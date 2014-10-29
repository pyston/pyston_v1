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

#include "codegen/ast_interpreter.h"

#include <unordered_map>

#include "analysis/function_analysis.h"
#include "analysis/scoping_analysis.h"
#include "codegen/codegen.h"
#include "codegen/irgen/hooks.h"
#include "codegen/irgen/irgenerator.h"
#include "codegen/irgen/util.h"
#include "codegen/osrentry.h"
#include "core/cfg.h"
#include "core/common.h"
#include "core/stats.h"
#include "core/thread_utils.h"
#include "core/util.h"
#include "runtime/generator.h"
#include "runtime/import.h"
#include "runtime/inline/boxing.h"
#include "runtime/long.h"
#include "runtime/set.h"
#include "runtime/types.h"

namespace pyston {

typedef std::vector<const ASTInterpreter::SymMap*> root_stack_t;
static threading::PerThreadSet<root_stack_t> root_stack_set;

static std::unordered_map<void*, ASTInterpreter*> s_interpreterMap;

Box* astInterpretFunction(CompiledFunction* cf, int nargs, Box* closure, Box* generator, Box* arg1, Box* arg2,
                          Box* arg3, Box** args) {
    if (unlikely(cf->times_called > 100)) {
        CompiledFunction* optimized = reoptCompiledFuncInternal(cf);
        if (closure && generator)
            return optimized->closure_generator_call((BoxedClosure*)closure, (BoxedGenerator*)generator, arg1, arg2,
                                                     arg3, args);
        else if (closure)
            return optimized->closure_call((BoxedClosure*)closure, arg1, arg2, arg3, args);
        else if (generator)
            return optimized->generator_call((BoxedGenerator*)generator, arg1, arg2, arg3, args);
        return optimized->call(arg1, arg2, arg3, args);
    }

    ++cf->times_called;

    ASTInterpreter interpreter(cf);
    s_interpreterMap[__builtin_frame_address(0)] = &interpreter;
    interpreter.initArguments(nargs, (BoxedClosure*)closure, (BoxedGenerator*)generator, arg1, arg2, arg3, args);
    Value v = interpreter.execute();

    s_interpreterMap.erase(__builtin_frame_address(0));

    return v.o ? v.o : None;
}

const LineInfo* getLineInfoForInterpretedFrame(void* frame_ptr) {
    ASTInterpreter* interpreter = s_interpreterMap[frame_ptr];
    assert(interpreter);
    SourceInfo* info = interpreter->source_info;
    LineInfo* line_info = new LineInfo(interpreter->current_inst->lineno, interpreter->current_inst->col_offset,
                                       info->parent_module->fn, info->getName());
    return line_info;
}

BoxedModule* getModuleForInterpretedFrame(void* frame_ptr) {
    ASTInterpreter* interpreter = s_interpreterMap[frame_ptr];
    assert(interpreter);
    return interpreter->source_info->parent_module;
}

BoxedDict* localsForInterpretedFrame(void* frame_ptr, bool only_user_visible) {
    ASTInterpreter* interpreter = s_interpreterMap[frame_ptr];
    assert(interpreter);
    BoxedDict* rtn = new BoxedDict();
    for (auto&& l : interpreter->sym_table) {
        if (only_user_visible && (l.getKey()[0] == '!' || l.getKey()[0] == '#'))
            continue;

        rtn->d[new BoxedString(l.getKey())] = l.getValue();
    }
    return rtn;
}

void gatherInterpreterRoots(GCVisitor* visitor) {
    root_stack_set.forEachValue(std::function<void(root_stack_t*, GCVisitor*)>([](root_stack_t* v, GCVisitor* visitor) {
                                    for (const ASTInterpreter::SymMap* sym_map : *v) {
                                        for (const auto& p2 : *sym_map) {
                                            visitor->visitPotential(p2.second);
                                        }
                                    }
                                }),
                                visitor);
}


ASTInterpreter::ASTInterpreter(CompiledFunction* compiled_function)
    : source_info(compiled_function->clfunc->source), next_block(0), current_block(0), current_inst(0),
      last_exception(0), passed_closure(0), created_closure(0), generator(0), scope_info(0), compiled_func(0),
      edgecount(0) {
    CLFunction* f = compiled_function->clfunc;
    if (!source_info->cfg)
        source_info->cfg = computeCFG(f->source, f->source->body);

    scope_info = source_info->getScopeInfo();
    root_stack_set.get()->push_back(&sym_table);
}

ASTInterpreter::~ASTInterpreter() {
    root_stack_set.get()->pop_back();
}

void ASTInterpreter::initArguments(int nargs, BoxedClosure* _closure, BoxedGenerator* _generator, Box* arg1, Box* arg2,
                                   Box* arg3, Box** args) {
    passed_closure = _closure;
    generator = _generator;

    if (scope_info->createsClosure())
        created_closure = createClosure(passed_closure);

    std::vector<Box*> argsArray{ arg1, arg2, arg3 };
    for (int i = 3; i < nargs; ++i)
        argsArray.push_back(args[i - 3]);

    int i = 0;
    if (source_info->arg_names.args) {
        for (AST_expr* e : *source_info->arg_names.args) {
            RELEASE_ASSERT(e->type == AST_TYPE::Name, "not implemented");
            AST_Name* name = (AST_Name*)e;
            doStore(name->id, argsArray[i++]);
        }
    }

    if (source_info->arg_names.vararg && !source_info->arg_names.vararg->empty()) {
        doStore(*source_info->arg_names.vararg, argsArray[i++]);
    }

    if (source_info->arg_names.kwarg && !source_info->arg_names.kwarg->empty()) {
        doStore(*source_info->arg_names.kwarg, argsArray[i++]);
    }
}

Value ASTInterpreter::execute(CFGBlock* block) {
    if (!block)
        block = source_info->cfg->getStartingBlock();

    Value v;
    next_block = block;
    while (next_block) {
        current_block = next_block;
        next_block = 0;

        for (AST_stmt* s : current_block->body) {
            current_inst = s;
            v = visit_stmt(s);
        }
    }
    return v;
}

void ASTInterpreter::eraseDeadSymbols() {
    if (source_info->liveness == NULL)
        source_info->liveness = computeLivenessInfo(source_info->cfg);

    if (source_info->phis == NULL)
        source_info->phis
            = computeRequiredPhis(source_info->arg_names, source_info->cfg, source_info->liveness, scope_info);

    std::vector<std::string> dead_symbols;
    for (auto&& it : sym_table) {
        if (!source_info->liveness->isLiveAtEnd(it.getKey(), current_block)) {
            dead_symbols.push_back(it.getKey());
        } else if (source_info->phis->isRequiredAfter(it.getKey(), current_block)) {
            assert(!scope_info->refersToGlobal(it.getKey()));
        } else {
        }
    }
    for (auto&& dead : dead_symbols)
        sym_table.erase(dead);
}

Value ASTInterpreter::doBinOp(Box* left, Box* right, int op, BinExpType exp_type) {
    if (op == AST_TYPE::Div && (source_info->parent_module->future_flags & FF_DIVISION)) {
        op = AST_TYPE::TrueDiv;
    }
    switch (exp_type) {
        case BinExpType::AugBinOp:
            return augbinop(left, right, op);
        case BinExpType::BinOp:
            return binop(left, right, op);
        case BinExpType::Compare:
            return compare(left, right, op);
        default:
            RELEASE_ASSERT(0, "not implemented");
    }
    return Value();
}

void ASTInterpreter::doStore(const std::string& name, Value value) {
    if (scope_info->refersToGlobal(name)) {
        setattr(source_info->parent_module, name.c_str(), value.o);
    } else {
        sym_table[name] = value.o;
        if (scope_info->saveInClosure(name))
            setattr(created_closure, name.c_str(), value.o);
    }
}

void ASTInterpreter::doStore(AST_expr* node, Value value) {
    if (node->type == AST_TYPE::Name) {
        AST_Name* name = (AST_Name*)node;
        doStore(name->id, value);
    } else if (node->type == AST_TYPE::Attribute) {
        AST_Attribute* attr = (AST_Attribute*)node;
        setattr(visit_expr(attr->value).o, attr->attr.c_str(), value.o);
    } else if (node->type == AST_TYPE::Tuple) {
        AST_Tuple* tuple = (AST_Tuple*)node;
        Box** array = unpackIntoArray(value.o, tuple->elts.size());
        unsigned i = 0;
        for (AST_expr* e : tuple->elts)
            doStore(e, array[i++]);
    } else if (node->type == AST_TYPE::List) {
        AST_List* list = (AST_List*)node;
        Box** array = unpackIntoArray(value.o, list->elts.size());
        unsigned i = 0;
        for (AST_expr* e : list->elts)
            doStore(e, array[i++]);
    } else if (node->type == AST_TYPE::Subscript) {
        AST_Subscript* subscript = (AST_Subscript*)node;

        Value target = visit_expr(subscript->value);
        Value slice = visit_expr(subscript->slice);

        setitem(target.o, slice.o, value.o);
    } else {
        RELEASE_ASSERT(0, "not implemented");
    }
}

Value ASTInterpreter::visit_unaryop(AST_UnaryOp* node) {
    Value operand = visit_expr(node->operand);
    if (node->op_type == AST_TYPE::Not)
        return boxBool(!nonzero(operand.o));
    else
        return unaryop(operand.o, node->op_type);
}

Value ASTInterpreter::visit_binop(AST_BinOp* node) {
    Value left = visit_expr(node->left);
    Value right = visit_expr(node->right);
    return doBinOp(left.o, right.o, node->op_type, BinExpType::BinOp);
}

Value ASTInterpreter::visit_slice(AST_Slice* node) {
    Value lower = node->lower ? visit_expr(node->lower) : None;
    Value upper = node->upper ? visit_expr(node->upper) : None;
    Value step = node->step ? visit_expr(node->step) : None;
    return createSlice(lower.o, upper.o, step.o);
}

Value ASTInterpreter::visit_branch(AST_Branch* node) {
    if (nonzero(visit_expr(node->test).o))
        next_block = node->iftrue;
    else
        next_block = node->iffalse;
    return Value();
}

Value ASTInterpreter::visit_jump(AST_Jump* node) {
    if (ENABLE_OSR && node->target->idx < current_block->idx && compiled_func) {
        ++edgecount;
        if (0 && edgecount > 10) {
            eraseDeadSymbols();

            OSRExit exit(compiled_func, OSREntryDescriptor::create(compiled_func, node));

            std::map<std::string, Box*> sorted_symbol_table;
            for (auto& l : sym_table)
                sorted_symbol_table[l.getKey()] = l.getValue();

            std::vector<Box*> arg_array;
            for (auto& it : sorted_symbol_table) {
                exit.entry->args[it.first] = UNKNOWN;
                arg_array.push_back(it.second);
            }

            CompiledFunction* partial_func = compilePartialFuncInternal(&exit);
            Box* arg1 = arg_array.size() >= 1 ? arg_array[0] : 0;
            Box* arg2 = arg_array.size() >= 2 ? arg_array[1] : 0;
            Box* arg3 = arg_array.size() >= 3 ? arg_array[2] : 0;
            Box** args = arg_array.size() >= 4 ? &arg_array[3] : 0;
            if (passed_closure && generator)
                return partial_func->closure_generator_call(passed_closure, generator, arg1, arg2, arg3, args);
            else if (passed_closure)
                return partial_func->closure_call(passed_closure, arg1, arg2, arg3, args);
            else if (generator)
                return partial_func->generator_call(generator, arg1, arg2, arg3, args);
            return partial_func->call(arg1, arg2, arg3, args);
        }
    }

    next_block = node->target;
    return Value();
}

Value ASTInterpreter::visit_invoke(AST_Invoke* node) {
    Value v;
    try {
        v = visit_stmt(node->stmt);
        next_block = node->normal_dest;
    } catch (Box* b) {
        next_block = node->exc_dest;
        last_exception = b;
    }

    return v;
}

Value ASTInterpreter::visit_clsAttribute(AST_ClsAttribute* node) {
    return getattr(visit_expr(node->value).o, node->attr.c_str());
}

Value ASTInterpreter::visit_augBinOp(AST_AugBinOp* node) {
    assert(node->op_type != AST_TYPE::Is && node->op_type != AST_TYPE::IsNot && "not tested yet");

    Value left = visit_expr(node->left);
    Value right = visit_expr(node->right);
    return doBinOp(left.o, right.o, node->op_type, BinExpType::AugBinOp);
}

Value ASTInterpreter::visit_langPrimitive(AST_LangPrimitive* node) {
    Value v;
    if (node->opcode == AST_LangPrimitive::GET_ITER) {
        assert(node->args.size() == 1);
        v = getiter(visit_expr(node->args[0]).o);
    } else if (node->opcode == AST_LangPrimitive::IMPORT_FROM) {
        assert(node->args.size() == 2);
        assert(node->args[0]->type == AST_TYPE::Name);
        assert(node->args[1]->type == AST_TYPE::Str);

        Value module = visit_expr(node->args[0]);
        const std::string& name = ast_cast<AST_Str>(node->args[1])->s;
        assert(name.size());
        v = importFrom(module.o, &name);
    } else if (node->opcode == AST_LangPrimitive::IMPORT_NAME) {
        assert(node->args.size() == 3);
        assert(node->args[0]->type == AST_TYPE::Num);
        assert(static_cast<AST_Num*>(node->args[0])->num_type == AST_Num::INT);
        assert(node->args[2]->type == AST_TYPE::Str);

        int level = static_cast<AST_Num*>(node->args[0])->n_int;
        Value froms = visit_expr(node->args[1]);
        const std::string& module_name = static_cast<AST_Str*>(node->args[2])->s;
        v = import(level, froms.o, &module_name);
    } else if (node->opcode == AST_LangPrimitive::IMPORT_STAR) {
        assert(node->args.size() == 1);
        assert(node->args[0]->type == AST_TYPE::Name);

        RELEASE_ASSERT(source_info->ast->type == AST_TYPE::Module, "import * not supported in functions");

        Value module = visit_expr(node->args[0]);
        v = importStar(module.o, source_info->parent_module);
    } else if (node->opcode == AST_LangPrimitive::NONE) {
        v = None;
    } else if (node->opcode == AST_LangPrimitive::LANDINGPAD) {
        v = last_exception;
        last_exception = nullptr;
    } else if (node->opcode == AST_LangPrimitive::ISINSTANCE) {
        assert(node->args.size() == 3);
        Value obj = visit_expr(node->args[0]);
        Value cls = visit_expr(node->args[1]);
        Value flags = visit_expr(node->args[2]);

        v = boxBool(isinstance(obj.o, cls.o, unboxInt(flags.o)));

    } else if (node->opcode == AST_LangPrimitive::LOCALS) {
        BoxedDict* dict = new BoxedDict;
        for (auto& p : sym_table) {
            llvm::StringRef s = p.first();
            if (s[0] == '!' || s[0] == '#')
                continue;

            dict->d[new BoxedString(s.str())] = p.second;
        }
        v = dict;
    } else
        RELEASE_ASSERT(0, "not implemented");
    return v;
}

Value ASTInterpreter::visit_yield(AST_Yield* node) {
    Value value = node->value ? visit_expr(node->value) : None;
    assert(generator && generator->cls == generator_cls);
    return yield(generator, value.o);
}

Value __attribute__((flatten)) ASTInterpreter::visit_stmt(AST_stmt* node) {
    switch (node->type) {
        case AST_TYPE::Assert:
            return visit_assert((AST_Assert*)node);
        case AST_TYPE::Assign:
            return visit_assign((AST_Assign*)node);
        case AST_TYPE::ClassDef:
            return visit_classDef((AST_ClassDef*)node);
        case AST_TYPE::Delete:
            return visit_delete((AST_Delete*)node);
        case AST_TYPE::Expr:
            return visit_expr((AST_Expr*)node);
        case AST_TYPE::FunctionDef:
            return visit_functionDef((AST_FunctionDef*)node);
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
    Value s(node->value ? visit_expr(node->value) : None);
    next_block = 0;
    return s;
}

Box* ASTInterpreter::createFunction(AST* node, AST_arguments* args, const std::vector<AST_stmt*>& body) {
    CLFunction* cl = wrapFunction(node, args, body, source_info);

    std::vector<Box*> defaults;
    for (AST_expr* d : args->defaults)
        defaults.push_back(visit_expr(d).o);
    defaults.push_back(0);

    struct {
        Box** ptr;
        size_t s;
    } d;

    d.ptr = &defaults[0];
    d.s = defaults.size() - 1;

    ScopeInfo* scope_info_node = source_info->scoping->getScopeInfoForNode(node);
    bool is_generator = scope_info_node->takesGenerator();

    BoxedClosure* closure = 0;
    if (scope_info_node->takesClosure()) {
        if (scope_info->createsClosure()) {
            closure = created_closure;
        } else {
            assert(scope_info->passesThroughClosure());
            closure = passed_closure;
        }
        assert(closure);
    }

    return boxCLFunction(cl, closure, is_generator, *(std::initializer_list<Box*>*)(void*)&d);
}

Value ASTInterpreter::visit_functionDef(AST_FunctionDef* node) {
    AST_arguments* args = node->args;

    std::vector<Box*> decorators;
    for (AST_expr* d : node->decorator_list)
        decorators.push_back(visit_expr(d).o);

    Box* func = createFunction(node, args, node->body);

    for (int i = decorators.size() - 1; i >= 0; i--)
        func = runtimeCall(decorators[i], ArgPassSpec(1), func, 0, 0, 0, 0);

    doStore(node->name, func);
    return Value();
}

Value ASTInterpreter::visit_classDef(AST_ClassDef* node) {
    ScopeInfo* scope_info = source_info->scoping->getScopeInfoForNode(node);
    assert(scope_info);

    BoxedTuple::GCVector bases;
    for (AST_expr* b : node->bases)
        bases.push_back(visit_expr(b).o);

    BoxedTuple* basesTuple = new BoxedTuple(std::move(bases));

    std::vector<Box*> decorators;
    for (AST_expr* d : node->decorator_list)
        decorators.push_back(visit_expr(d).o);

    BoxedClosure* closure = scope_info->takesClosure() ? created_closure : 0;
    CLFunction* cl = wrapFunction(node, nullptr, node->body, source_info);
    Box* attrDict = runtimeCall(boxCLFunction(cl, closure, false, {}), ArgPassSpec(0), 0, 0, 0, 0, 0);

    Box* classobj = createUserClass(&node->name, basesTuple, attrDict);

    for (int i = decorators.size() - 1; i >= 0; i--)
        classobj = runtimeCall(decorators[i], ArgPassSpec(1), classobj, 0, 0, 0, 0);

    doStore(node->name, classobj);
    return Value();
}

Value ASTInterpreter::visit_raise(AST_Raise* node) {
    if (node->arg0 == NULL) {
        assert(!node->arg1);
        assert(!node->arg2);
        raise0();
    }

    raise3(node->arg0 ? visit_expr(node->arg0).o : None, node->arg1 ? visit_expr(node->arg1).o : None,
           node->arg2 ? visit_expr(node->arg2).o : None);
    return Value();
}

Value ASTInterpreter::visit_assert(AST_Assert* node) {
    if (!nonzero(visit_expr(node->test).o))
        assertFail(source_info->parent_module, node->msg ? visit_expr(node->msg).o : 0);
    return Value();
}

Value ASTInterpreter::visit_global(AST_Global* node) {
    for (std::string& name : node->names)
        sym_table.erase(name);
    return Value();
}

Value ASTInterpreter::visit_delete(AST_Delete* node) {
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
                delattr(visit_expr(attr->value).o, attr->attr.c_str());
                break;
            }
            case AST_TYPE::Name: {
                AST_Name* target = (AST_Name*)target_;
                if (scope_info->refersToGlobal(target->id)) {
                    // Can't use delattr since the errors are different:
                    delGlobal(source_info->parent_module, &target->id);
                    continue;
                }

                assert(!scope_info->refersToClosure(target->id));
                assert(!scope_info->saveInClosure(
                    target->id)); // SyntaxError: can not delete variable 'x' referenced in nested scope

                // A del of a missing name generates different error messages in a function scope vs a classdef scope
                bool local_error_msg = (source_info->ast->type != AST_TYPE::ClassDef);

                if (sym_table.count(target->id) == 0) {
                    assertNameDefined(0, target->id.c_str(), NameError, local_error_msg);
                    return Value();
                }

                sym_table.erase(target->id);
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
    Value v = visit_expr(node->value);
    for (AST_expr* e : node->targets)
        doStore(e, v);
    return Value();
}

Value ASTInterpreter::visit_print(AST_Print* node) {
    static const std::string write_str("write");
    static const std::string newline_str("\n");
    static const std::string space_str(" ");

    Box* dest = node->dest ? visit_expr(node->dest).o : getSysStdout();
    int nvals = node->values.size();
    for (int i = 0; i < nvals; i++) {
        Box* var = visit_expr(node->values[i]).o;

        // begin code for handling of softspace
        bool new_softspace = (i < nvals - 1) || (!node->nl);
        if (softspace(dest, new_softspace)) {
            callattrInternal(dest, &write_str, CLASS_OR_INST, 0, ArgPassSpec(1), boxString(space_str), 0, 0, 0, 0);
        }
        callattrInternal(dest, &write_str, CLASS_OR_INST, 0, ArgPassSpec(1), str(var), 0, 0, 0, 0);
    }

    if (node->nl) {
        callattrInternal(dest, &write_str, CLASS_OR_INST, 0, ArgPassSpec(1), boxString(newline_str), 0, 0, 0, 0);
        if (nvals == 0) {
            softspace(dest, false);
        }
    }
    return Value();
}

Value ASTInterpreter::visit_compare(AST_Compare* node) {
    RELEASE_ASSERT(node->comparators.size() == 1, "not implemented");
    return doBinOp(visit_expr(node->left).o, visit_expr(node->comparators[0]).o, node->ops[0], BinExpType::Compare);
}

Value __attribute__((flatten)) ASTInterpreter::visit_expr(AST_expr* node) {
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
        default:
            RELEASE_ASSERT(0, "");
    };
    return Value();
}


Value ASTInterpreter::visit_call(AST_Call* node) {
    Value v;
    Value func;

    std::string* attr = nullptr;

    bool is_callattr = false;
    bool callattr_clsonly = false;
    if (node->func->type == AST_TYPE::Attribute) {
        is_callattr = true;
        callattr_clsonly = false;
        AST_Attribute* attr_ast = ast_cast<AST_Attribute>(node->func);
        func = visit_expr(attr_ast->value);
        attr = &attr_ast->attr;
    } else if (node->func->type == AST_TYPE::ClsAttribute) {
        is_callattr = true;
        callattr_clsonly = true;
        AST_ClsAttribute* attr_ast = ast_cast<AST_ClsAttribute>(node->func);
        func = visit_expr(attr_ast->value);
        attr = &attr_ast->attr;
    } else
        func = visit_expr(node->func);

    std::vector<Box*> args;
    for (AST_expr* e : node->args)
        args.push_back(visit_expr(e).o);

    std::vector<const std::string*> keywords;
    for (AST_keyword* k : node->keywords) {
        keywords.push_back(&k->arg);
        args.push_back(visit_expr(k->value).o);
    }

    if (node->starargs)
        args.push_back(visit_expr(node->starargs).o);

    if (node->kwargs)
        args.push_back(visit_expr(node->kwargs).o);

    ArgPassSpec argspec(node->args.size(), node->keywords.size(), node->starargs, node->kwargs);

    if (is_callattr) {
        return callattr(func.o, attr, CallattrFlags({.cls_only = callattr_clsonly, .null_on_nonexistent = false }),
                        argspec, args.size() > 0 ? args[0] : 0, args.size() > 1 ? args[1] : 0,
                        args.size() > 2 ? args[2] : 0, args.size() > 3 ? &args[3] : 0, &keywords);
    } else {
        return runtimeCall(func.o, argspec, args.size() > 0 ? args[0] : 0, args.size() > 1 ? args[1] : 0,
                           args.size() > 2 ? args[2] : 0, args.size() > 3 ? &args[3] : 0, &keywords);
    }
}


Value ASTInterpreter::visit_expr(AST_Expr* node) {
    return visit_expr(node->value);
}

Value ASTInterpreter::visit_num(AST_Num* node) {
    if (node->num_type == AST_Num::INT)
        return boxInt(node->n_int);
    else if (node->num_type == AST_Num::FLOAT)
        return boxFloat(node->n_float);
    else if (node->num_type == AST_Num::LONG)
        return createLong(&node->n_long);
    else if (node->num_type == AST_Num::COMPLEX)
        return boxComplex(0.0, node->n_float);
    RELEASE_ASSERT(0, "not implemented");
    return Value();
}

Value ASTInterpreter::visit_index(AST_Index* node) {
    return visit_expr(node->value);
}

Value ASTInterpreter::visit_repr(AST_Repr* node) {
    return repr(visit_expr(node->value).o);
}

Value ASTInterpreter::visit_lambda(AST_Lambda* node) {
    AST_Return* expr = new AST_Return();
    expr->value = node->body;

    std::vector<AST_stmt*> body = { expr };
    return createFunction(node, node->args, body);
}

Value ASTInterpreter::visit_dict(AST_Dict* node) {
    RELEASE_ASSERT(node->keys.size() == node->values.size(), "not implemented");
    BoxedDict* dict = new BoxedDict();
    for (size_t i = 0; i < node->keys.size(); ++i) {
        Box* v = visit_expr(node->values[i]).o;
        Box* k = visit_expr(node->keys[i]).o;
        dict->d[k] = v;
    }

    return dict;
}

Value ASTInterpreter::visit_set(AST_Set* node) {
    BoxedSet::Set set;
    for (AST_expr* e : node->elts)
        set.insert(visit_expr(e).o);

    return new BoxedSet(std::move(set), set_cls);
}

Value ASTInterpreter::visit_str(AST_Str* node) {
    return boxString(node->s);
}

Value ASTInterpreter::visit_name(AST_Name* node) {
    if (scope_info->refersToGlobal(node->id))
        return getGlobal(source_info->parent_module, &node->id);
    else if (scope_info->refersToClosure(node->id)) {
        return getattr(passed_closure, node->id.c_str());
    } else {
        SymMap::iterator it = sym_table.find(node->id);
        if (it != sym_table.end()) {
            Box* value = it->second;
            if (!value)
                assertNameDefined(value, node->id.c_str(), UnboundLocalError, true);
            return value;
        }

        // classdefs have different scoping rules than functions:
        if (source_info->ast->type == AST_TYPE::ClassDef)
            return getGlobal(source_info->parent_module, &node->id);

        assertNameDefined(0, node->id.c_str(), UnboundLocalError, true);
        return Value();
    }
}


Value ASTInterpreter::visit_subscript(AST_Subscript* node) {
    Value value = visit_expr(node->value);
    Value slice = visit_expr(node->slice);
    return getitem(value.o, slice.o);
}

Value ASTInterpreter::visit_list(AST_List* node) {
    BoxedList* list = new BoxedList;
    list->ensure(node->elts.size());
    for (AST_expr* e : node->elts)
        listAppendInternal(list, visit_expr(e).o);
    return list;
}

Value ASTInterpreter::visit_tuple(AST_Tuple* node) {
    BoxedTuple::GCVector elts;
    for (AST_expr* e : node->elts)
        elts.push_back(visit_expr(e).o);
    return new BoxedTuple(std::move(elts));
}

Value ASTInterpreter::visit_attribute(AST_Attribute* node) {
    return getattr(visit_expr(node->value).o, node->attr.c_str());
}
}
