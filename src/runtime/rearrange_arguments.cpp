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

#include "core/types.h"
#include "runtime/dict.h"
#include "runtime/objmodel.h"
#include "runtime/rewrite_args.h"
#include "runtime/types.h"

namespace pyston {

// The main function here is rearrangeArguments
// which deals with moving the args passed into the form
// the function expects to be passed in (i.e. it handles starargs
// and kwargs).
// The logic is already complex, and the function also deals with rewriting
// the logic for ICs, so it gets pretty hairy.

enum class KeywordDest {
    POSITIONAL,
    KWARGS,
};
static KeywordDest placeKeyword(const ParamNames* param_names, llvm::SmallVector<bool, 8>& params_filled,
                                BoxedString* kw_name, Box* kw_val, Box*& oarg1, Box*& oarg2, Box*& oarg3, Box** oargs,
                                BoxedDict* okwargs, const char* func_name) {
    assert(kw_val);
    assert(gc::isValidGCObject(kw_val));
    assert(kw_name);
    assert(gc::isValidGCObject(kw_name));

    for (int j = 0; j < param_names->args.size(); j++) {
        if (param_names->args[j] == kw_name->s() && kw_name->size() > 0) {
            if (params_filled[j]) {
                raiseExcHelper(TypeError, "%.200s() got multiple values for keyword argument '%s'", func_name,
                               kw_name->c_str());
            }

            getArg(j, oarg1, oarg2, oarg3, oargs) = kw_val;
            params_filled[j] = true;

            return KeywordDest::POSITIONAL;
        }
    }

    if (okwargs) {
        Box*& v = okwargs->d[kw_name];
        if (v) {
            raiseExcHelper(TypeError, "%.200s() got multiple values for keyword argument '%s'", func_name,
                           kw_name->c_str());
        }
        v = kw_val;
        return KeywordDest::KWARGS;
    } else {
        raiseExcHelper(TypeError, "%.200s() got an unexpected keyword argument '%s'", func_name, kw_name->c_str());
    }
}

// unpacks the passed stararg
// callable from the IC
// TODO we can specialize further when given_varargs is a list or tuple
// (e.g. just copy elements inline)
// throws the appropriate exception on failure
// if exactly 0 arguments are expected, args_out can be NULL
template <bool takesStarParam>
static inline void fillArgsFromStarArg(Box** args_out, Box* given_varargs, ArgPassSpec argspec,
                                       ParamReceiveSpec paramspec, const char* fname) {
    llvm::SmallVector<Box*, 8> starParamElts;

    int numParams = paramspec.num_args - argspec.num_args;

    int i = 0;
    for (Box* e : given_varargs->pyElements()) {
        if (i >= numParams) {
            if (takesStarParam) {
                starParamElts.push_back(e);
            } else {
                i++;
            }
        } else {
            assert(args_out);
            args_out[i] = e;
            i++;
        }
    }

    if (i < numParams || (i > numParams && !takesStarParam)) {
        if (takesStarParam) {
            raiseExcHelper(TypeError, takesStarParam ? "%s() takes at least %d argument%s (%d given)"
                                                     : "%s() takes exactly %d argument%s (%d given)",
                           fname, paramspec.num_args, paramspec.num_args == 1 ? "" : "s", argspec.num_args + i);
        }
    }

    if (takesStarParam) {
        BoxedTuple* starParam = BoxedTuple::create(starParamElts.size());
        for (int i = 0; i < starParamElts.size(); i++) {
            starParam->elts[i] = starParamElts[i];
        }
        assert(args_out);
        args_out[numParams] = starParam;
    }
}
extern "C" void fillArgsFromStarArgNoStarParam(Box** args_out, Box* given_varargs, ArgPassSpec argspec,
                                               ParamReceiveSpec paramspec, const char* fname) {
    fillArgsFromStarArg<false>(args_out, given_varargs, argspec, paramspec, fname);
}
extern "C" void fillArgsFromStarArgWithStarParam(Box** args_out, Box* given_varargs, ArgPassSpec argspec,
                                                 ParamReceiveSpec paramspec, const char* fname) {
    fillArgsFromStarArg<true>(args_out, given_varargs, argspec, paramspec, fname);
}

// A helper function for dealing with the case where starargs is given,
// and unused positional parameters go in with the starargs to make the varargs parameter
extern "C" BoxedTuple* makeVarArgsFromArgsAndStarArgs(Box* arg1, Box* arg2, Box* arg3, Box** args, ArgPassSpec argspec,
                                                      ParamReceiveSpec paramspec) {
    assert(argspec.num_args >= paramspec.num_args);
    assert(argspec.has_starargs);
    assert(paramspec.takes_varargs);

    llvm::SmallVector<Box*, 8> starParamElts;

    for (int i = paramspec.num_args; i < argspec.num_args; i++) {
        starParamElts.push_back(getArg(i, arg1, arg2, arg3, args));
    }

    Box* given_varargs = getArg(argspec.num_args, arg1, arg2, arg3, args);
    for (Box* e : given_varargs->pyElements()) {
        starParamElts.push_back(e);
    }

    BoxedTuple* starParam = BoxedTuple::create(starParamElts.size());
    for (int i = 0; i < starParamElts.size(); i++) {
        starParam->elts[i] = starParamElts[i];
    }
    return starParam;
}

void rearrangeArguments(ParamReceiveSpec paramspec, const ParamNames* param_names, const char* func_name,
                        Box** defaults, CallRewriteArgs* rewrite_args, bool& rewrite_success, ArgPassSpec argspec,
                        Box* arg1, Box* arg2, Box* arg3, Box** args, const std::vector<BoxedString*>* keyword_names,
                        Box*& oarg1, Box*& oarg2, Box*& oarg3, Box** oargs) {

    /*
     * Procedure:
     * - First match up positional arguments; any extra go to varargs.  error if too many.
     * - Then apply keywords; any extra go to kwargs.  error if too many.
     * - Use defaults to fill in any missing
     * - error about missing parameters
     */

    int num_output_args = paramspec.totalReceived();
    int num_passed_args = argspec.totalPassed();

    if (num_passed_args >= 1)
        assert(gc::isValidGCObject(arg1) || !arg1);
    if (num_passed_args >= 2)
        assert(gc::isValidGCObject(arg2) || !arg2);
    if (num_passed_args >= 3)
        assert(gc::isValidGCObject(arg3) || !arg3);
    for (int i = 3; i < num_passed_args; i++) {
        assert(gc::isValidGCObject(args[i - 3]) || args[i - 3] == NULL);
    }

    assert((defaults != NULL) == (paramspec.num_defaults != 0));

    if (rewrite_args) {
        rewrite_success = false; // default case
    }

    // Fast path: if it's a simple-enough call, we don't have to do anything special.  On a simple
    // django-admin test this covers something like 93% of all calls to callFunc.
    if (argspec.num_keywords == 0 && argspec.has_starargs == paramspec.takes_varargs && !argspec.has_kwargs
        && !paramspec.takes_kwargs && argspec.num_args == paramspec.num_args) {
        assert(num_output_args == num_passed_args);

        // If the caller passed starargs, we can only pass those directly to the callee if it's a tuple,
        // since otherwise modifications by the callee would be visible to the caller (hence why varargs
        // received by the caller are always tuples).
        // This is why we can't pass kwargs here.
        if (argspec.has_starargs) {
            Box* given_varargs = getArg(argspec.num_args + argspec.num_keywords, arg1, arg2, arg3, args);
            if (given_varargs->cls == tuple_cls) {
                if (rewrite_args) {
                    rewrite_args->getArg(argspec.num_args + argspec.num_keywords)
                        ->addAttrGuard(offsetof(Box, cls), (intptr_t)tuple_cls);
                }
                rewrite_success = true;
                oarg1 = arg1;
                oarg2 = arg2;
                oarg3 = arg3;
                if (num_output_args > 3)
                    memcpy(oargs, args, sizeof(Box*) * (num_output_args - 3));
                return;
            }
        } else {
            rewrite_success = true;
            oarg1 = arg1;
            oarg2 = arg2;
            oarg3 = arg3;
            if (num_output_args > 3)
                memcpy(oargs, args, sizeof(Box*) * (num_output_args - 3));
            return;
        }
    }

    // General case

    static StatCounter slowpath_rearrangeargs_slowpath("slowpath_rearrangeargs_slowpath");
    slowpath_rearrangeargs_slowpath.log();

    std::vector<Box*, StlCompatAllocator<Box*>> varargs;

    if (argspec.has_starargs) {
        Box* given_varargs = getArg(argspec.num_args + argspec.num_keywords, arg1, arg2, arg3, args);
        for (Box* e : given_varargs->pyElements()) {
            varargs.push_back(e);
        }
    }

    ////
    // First, match up positional parameters to positional/varargs:
    int positional_to_positional = std::min(argspec.num_args, paramspec.num_args);
    for (int i = 0; i < positional_to_positional; i++) {
        getArg(i, oarg1, oarg2, oarg3, oargs) = getArg(i, arg1, arg2, arg3, args);
    }

    int varargs_to_positional = std::min((int)varargs.size(), paramspec.num_args - positional_to_positional);
    for (int i = 0; i < varargs_to_positional; i++) {
        getArg(i + positional_to_positional, oarg1, oarg2, oarg3, oargs) = varargs[i];
    }

    llvm::SmallVector<bool, 8> params_filled(num_output_args);
    for (int i = 0; i < positional_to_positional + varargs_to_positional; i++) {
        params_filled[i] = true;
    }

    std::vector<Box*, StlCompatAllocator<Box*>> unused_positional;
    RewriterVar::SmallVector unused_positional_rvars;
    for (int i = positional_to_positional; i < argspec.num_args; i++) {
        unused_positional.push_back(getArg(i, arg1, arg2, arg3, args));
    }
    for (int i = varargs_to_positional; i < varargs.size(); i++) {
        unused_positional.push_back(varargs[i]);
    }

    if (paramspec.takes_varargs) {
        int varargs_idx = paramspec.num_args;
        Box* ovarargs = BoxedTuple::create(unused_positional.size(), &unused_positional[0]);
        getArg(varargs_idx, oarg1, oarg2, oarg3, oargs) = ovarargs;
    } else if (unused_positional.size()) {
        raiseExcHelper(TypeError, "%s() takes at most %d argument%s (%d given)", func_name, paramspec.num_args,
                       (paramspec.num_args == 1 ? "" : "s"), argspec.num_args + argspec.num_keywords + varargs.size());
    }

    ////
    // Second, apply any keywords:

    BoxedDict* okwargs = NULL;
    if (paramspec.takes_kwargs) {
        int kwargs_idx = paramspec.num_args + (paramspec.takes_varargs ? 1 : 0);
        okwargs = new BoxedDict();
        getArg(kwargs_idx, oarg1, oarg2, oarg3, oargs) = okwargs;
    }

    if ((!param_names || !param_names->takes_param_names) && argspec.num_keywords && !paramspec.takes_kwargs) {
        raiseExcHelper(TypeError, "%s() doesn't take keyword arguments", func_name);
    }

    if (argspec.num_keywords)
        assert(argspec.num_keywords == keyword_names->size());

    for (int i = 0; i < argspec.num_keywords; i++) {
        int arg_idx = i + argspec.num_args;
        Box* kw_val = getArg(arg_idx, arg1, arg2, arg3, args);

        if (!param_names || !param_names->takes_param_names) {
            assert(okwargs);
            okwargs->d[(*keyword_names)[i]] = kw_val;
            continue;
        }

        auto dest = placeKeyword(param_names, params_filled, (*keyword_names)[i], kw_val, oarg1, oarg2, oarg3, oargs,
                                 okwargs, func_name);
    }

    if (argspec.has_kwargs) {
        Box* kwargs
            = getArg(argspec.num_args + argspec.num_keywords + (argspec.has_starargs ? 1 : 0), arg1, arg2, arg3, args);

        if (!isSubclass(kwargs->cls, dict_cls)) {
            BoxedDict* d = new BoxedDict();
            dictMerge(d, kwargs);
            kwargs = d;
        }
        assert(isSubclass(kwargs->cls, dict_cls));
        BoxedDict* d_kwargs = static_cast<BoxedDict*>(kwargs);

        for (auto& p : d_kwargs->d) {
            auto k = coerceUnicodeToStr(p.first);

            if (k->cls != str_cls)
                raiseExcHelper(TypeError, "%s() keywords must be strings", func_name);

            BoxedString* s = static_cast<BoxedString*>(k);

            if (param_names && param_names->takes_param_names) {
                placeKeyword(param_names, params_filled, s, p.second, oarg1, oarg2, oarg3, oargs, okwargs, func_name);
            } else {
                assert(okwargs);

                Box*& v = okwargs->d[p.first];
                if (v) {
                    raiseExcHelper(TypeError, "%s() got multiple values for keyword argument '%s'", func_name,
                                   s->data());
                }
                v = p.second;
            }
        }
    }

    // Fill with defaults:

    for (int i = 0; i < paramspec.num_args - paramspec.num_defaults; i++) {
        if (params_filled[i])
            continue;
        // TODO not right error message
        raiseExcHelper(TypeError, "%s() did not get a value for positional argument %d", func_name, i);
    }

    for (int arg_idx = paramspec.num_args - paramspec.num_defaults; arg_idx < paramspec.num_args; arg_idx++) {
        if (params_filled[arg_idx])
            continue;

        int default_idx = arg_idx + paramspec.num_defaults - paramspec.num_args;
        Box* default_obj = defaults[default_idx];
        getArg(arg_idx, oarg1, oarg2, oarg3, oargs) = default_obj;
    }

    if (argspec.has_starargs) {
        static StatCounter sc("slowpath_rearrange_args_has_starargs_no_exception");
        sc.log();
    }

    if (!rewrite_args)
        return;

    ///////////////////////////////
    // Now do all the rewriting

    // Right now we don't handle either of these
    if (argspec.has_kwargs || argspec.num_keywords)
        return;

    if (argspec.has_starargs && !paramspec.num_defaults && !paramspec.takes_kwargs) {
        assert(!argspec.has_kwargs);
        assert(!argspec.num_keywords);
        // We just dispatch to a helper function to copy the args and call pyElements
        // TODO In some cases we can be smarter depending on the arrangement of args and type
        // of the star args object. For example if star args is an (immutable) tuple,
        // we may be able to have the `Box** args` pointer point directly into it.
        if (argspec.num_args > paramspec.num_args) {
            assert(paramspec.takes_varargs);

            RewriterVar::SmallVector callargs;
            callargs.push_back(rewrite_args->arg1 ? rewrite_args->arg1 : rewrite_args->rewriter->loadConst(0));
            callargs.push_back(rewrite_args->arg2 ? rewrite_args->arg2 : rewrite_args->rewriter->loadConst(0));
            callargs.push_back(rewrite_args->arg3 ? rewrite_args->arg3 : rewrite_args->rewriter->loadConst(0));
            callargs.push_back(rewrite_args->args ? rewrite_args->args : rewrite_args->rewriter->loadConst(0));
            callargs.push_back(rewrite_args->rewriter->loadConst(argspec.asInt()));
            callargs.push_back(rewrite_args->rewriter->loadConst(paramspec.asInt()));
            RewriterVar* r_varargs = rewrite_args->rewriter->call(true /* has side effects */,
                                                                  (void*)makeVarArgsFromArgsAndStarArgs, callargs);

            if (paramspec.num_args == 0)
                rewrite_args->arg1 = r_varargs;
            else if (paramspec.num_args == 1)
                rewrite_args->arg2 = r_varargs;
            else if (paramspec.num_args == 2)
                rewrite_args->arg3 = r_varargs;
            else {
                rewrite_args->args = rewrite_args->rewriter->allocateAndCopy(rewrite_args->args, num_output_args - 3);
                rewrite_args->args->setAttr(sizeof(Box*) * (paramspec.num_args - 3), r_varargs);
            }
        } else {
            if (argspec.num_args <= 3) {
                assert(paramspec.num_args >= argspec.num_args);
                int bufSize = paramspec.num_args - argspec.num_args + (paramspec.takes_varargs ? 1 : 0);
                RewriterVar* r_buf_ptr = bufSize > 0 ? rewrite_args->rewriter->allocate(bufSize)
                                                     : rewrite_args->rewriter->loadConst(0);
                rewrite_args->rewriter->call(true /* has side effects */,
                                             (void*)(paramspec.takes_varargs ? fillArgsFromStarArgWithStarParam
                                                                             : fillArgsFromStarArgNoStarParam),
                                             r_buf_ptr, rewrite_args->getArg(argspec.num_args),
                                             rewrite_args->rewriter->loadConst(argspec.asInt()),
                                             rewrite_args->rewriter->loadConst(paramspec.asInt()),
                                             rewrite_args->rewriter->loadConst((int64_t)func_name));
                for (int i = argspec.num_args; i < (paramspec.num_args + (paramspec.takes_varargs ? 1 : 0)); i++) {
                    int buf_offset = sizeof(Box*) * (i - argspec.num_args);
                    if (i == 0)
                        rewrite_args->arg1 = r_buf_ptr->getAttr(buf_offset);
                    else if (i == 1)
                        rewrite_args->arg2 = r_buf_ptr->getAttr(buf_offset);
                    else if (i == 2)
                        rewrite_args->arg3 = r_buf_ptr->getAttr(buf_offset);
                    else {
                        assert(i == 3);
                        rewrite_args->args = rewrite_args->rewriter->add(r_buf_ptr, buf_offset);
                        break;
                    }
                }
            } else {
                assert(argspec.num_args >= 3);
                assert(paramspec.num_args + (paramspec.takes_varargs ? 1 : 0) >= 3);
                RewriterVar* r_buf_ptr = rewrite_args->rewriter->allocateAndCopy(
                    rewrite_args->args, argspec.num_args - 3,
                    paramspec.num_args + (paramspec.takes_varargs ? 1 : 0) - 3);

                RewriterVar* r_buf_ptr_for_varargs
                    = rewrite_args->rewriter->add(r_buf_ptr, (argspec.num_args - 3) * sizeof(Box*), assembler::RDI);

                rewrite_args->rewriter->call(true /* has side effects */,
                                             (void*)(paramspec.takes_varargs ? fillArgsFromStarArgWithStarParam
                                                                             : fillArgsFromStarArgNoStarParam),
                                             r_buf_ptr_for_varargs, rewrite_args->getArg(argspec.num_args),
                                             rewrite_args->rewriter->loadConst(argspec.asInt()),
                                             rewrite_args->rewriter->loadConst(paramspec.asInt()),
                                             rewrite_args->rewriter->loadConst((int64_t)func_name));

                rewrite_args->args = r_buf_ptr;
            }
        }

        rewrite_success = true;
        return;
    }

    if (!(paramspec.takes_varargs && argspec.num_args > paramspec.num_args + 3) && !argspec.has_starargs) {
        // We might have trouble if we have more output args than input args,
        // such as if we need more space to pass defaults.
        bool did_copy = false;
        if (num_output_args > 3 && num_output_args > num_passed_args) {
            int arg_bytes_required = (num_output_args - 3) * sizeof(Box*);
            RewriterVar* new_args = NULL;

            assert((rewrite_args->args == NULL) == (num_passed_args <= 3));
            if (num_passed_args <= 3) {
                // we weren't passed args
                new_args = rewrite_args->rewriter->allocate(num_output_args - 3);
            } else {
                new_args = rewrite_args->rewriter->allocateAndCopy(rewrite_args->args, num_passed_args - 3,
                                                                   num_output_args - 3);
            }

            rewrite_args->args = new_args;

            did_copy = true;
        }

        RewriterVar::SmallVector unused_positional_rvars;
        for (int i = positional_to_positional; i < argspec.num_args; i++) {
            unused_positional_rvars.push_back(rewrite_args->getArg(i));
        }

        if (paramspec.takes_varargs) {
            int varargs_idx = paramspec.num_args;
            assert(!varargs.size());

            RewriterVar* varargs_val;
            int varargs_size = unused_positional_rvars.size();

            if (varargs_size == 0) {
                varargs_val = rewrite_args->rewriter->loadConst(
                    (intptr_t)EmptyTuple, varargs_idx < 3 ? Location::forArg(varargs_idx) : Location::any());
            } else if (varargs_size == 1) {
                varargs_val
                    = rewrite_args->rewriter->call(false, (void*)BoxedTuple::create1, unused_positional_rvars[0]);
            } else if (varargs_size == 2) {
                varargs_val = rewrite_args->rewriter->call(false, (void*)BoxedTuple::create2,
                                                           unused_positional_rvars[0], unused_positional_rvars[1]);
            } else if (varargs_size == 3) {
                varargs_val
                    = rewrite_args->rewriter->call(false, (void*)BoxedTuple::create3, unused_positional_rvars[0],
                                                   unused_positional_rvars[1], unused_positional_rvars[2]);
            } else {
                // This is too late to abort the rewrite (we should have checked this earlier)
                abort();
            }

            if (varargs_val) {
                if (varargs_idx == 0)
                    rewrite_args->arg1 = varargs_val;
                if (varargs_idx == 1)
                    rewrite_args->arg2 = varargs_val;
                if (varargs_idx == 2)
                    rewrite_args->arg3 = varargs_val;
                if (varargs_idx >= 3) {
                    if (!did_copy) {
                        if (num_passed_args <= 3) {
                            // we weren't passed args
                            rewrite_args->args = rewrite_args->rewriter->allocate(num_output_args - 3);
                        } else {
                            rewrite_args->args
                                = rewrite_args->rewriter->allocateAndCopy(rewrite_args->args, num_output_args - 3);
                        }
                        did_copy = true;
                    }

                    rewrite_args->args->setAttr((varargs_idx - 3) * sizeof(Box*), varargs_val);
                }
            }
        }

        if (paramspec.takes_kwargs) {
            assert(!argspec.num_keywords && !argspec.has_kwargs);

            int kwargs_idx = paramspec.num_args + (paramspec.takes_varargs ? 1 : 0);
            RewriterVar* r_kwargs = rewrite_args->rewriter->call(true, (void*)createDict);

            if (kwargs_idx == 0)
                rewrite_args->arg1 = r_kwargs;
            if (kwargs_idx == 1)
                rewrite_args->arg2 = r_kwargs;
            if (kwargs_idx == 2)
                rewrite_args->arg3 = r_kwargs;
            if (kwargs_idx >= 3) {
                assert(did_copy);
                rewrite_args->args->setAttr((kwargs_idx - 3) * sizeof(Box*), r_kwargs);
            }
        }

        for (int arg_idx = std::max((int)(paramspec.num_args - paramspec.num_defaults), (int)argspec.num_args);
             arg_idx < paramspec.num_args; arg_idx++) {
            int default_idx = arg_idx + paramspec.num_defaults - paramspec.num_args;

            Box* default_obj = defaults[default_idx];

            if (arg_idx == 0)
                rewrite_args->arg1 = rewrite_args->rewriter->loadConst((intptr_t)default_obj, Location::forArg(0));
            else if (arg_idx == 1)
                rewrite_args->arg2 = rewrite_args->rewriter->loadConst((intptr_t)default_obj, Location::forArg(1));
            else if (arg_idx == 2)
                rewrite_args->arg3 = rewrite_args->rewriter->loadConst((intptr_t)default_obj, Location::forArg(2));
            else {
                assert(did_copy);
                rewrite_args->args->setAttr((arg_idx - 3) * sizeof(Box*),
                                            rewrite_args->rewriter->loadConst((intptr_t)default_obj));
            }
        }

        rewrite_success = true;
        return;
    }
}
}
