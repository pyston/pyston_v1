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

#include <cassert>
#include <cstdio>
#include <stdint.h>
#include <cstdlib>
#include <cstring>
#include <memory>

#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"

#include "core/ast.h"
#include "core/options.h"
#include "core/stats.h"
#include "core/types.h"

#include "codegen/parser.h"
#include "codegen/type_recording.h"
#include "codegen/irgen/hooks.h"

#include "asm_writing/icinfo.h"
#include "asm_writing/rewriter.h"
#include "asm_writing/rewriter2.h"

#include "runtime/capi.h"
#include "runtime/float.h"
#include "runtime/gc_runtime.h"
#include "runtime/objmodel.h"
#include "runtime/types.h"
#include "runtime/util.h"

#define BOX_NREFS_OFFSET ((char*)&(((HCBox*)0x01)->nrefs) - (char*)0x1)
#define BOX_CLS_OFFSET ((char*)&(((HCBox*)0x01)->cls) - (char*)0x1)
#define BOX_HCLS_OFFSET ((char*)&(((HCBox*)0x01)->hcls) - (char*)0x1)
#define BOX_ATTRS_OFFSET ((char*)&(((HCBox*)0x01)->attr_list) - (char*)0x1)
#define ATTRLIST_ATTRS_OFFSET ((char*)&(((HCBox::AttrList*)0x01)->attrs) - (char*)0x1)
#define ATTRLIST_KIND_OFFSET ((char*)&(((HCBox::AttrList*)0x01)->gc_header.kind_id) - (char*)0x1)
#define INSTANCEMETHOD_FUNC_OFFSET ((char*)&(((BoxedInstanceMethod*)0x01)->func) - (char*)0x1)
#define INSTANCEMETHOD_OBJ_OFFSET ((char*)&(((BoxedInstanceMethod*)0x01)->obj) - (char*)0x1)
#define BOOL_B_OFFSET ((char*)&(((BoxedBool*)0x01)->b) - (char*)0x1)
#define INT_N_OFFSET ((char*)&(((BoxedInt*)0x01)->n) - (char*)0x1)

namespace pyston {

struct GetattrRewriteArgs {
    Rewriter *rewriter;
    RewriterVar obj;
    bool out_success;
    RewriterVar out_rtn;

    bool obj_hcls_guarded;
    int preferred_dest_reg;

    GetattrRewriteArgs(Rewriter *rewriter, const RewriterVar &obj) :
            rewriter(rewriter), obj(obj), out_success(false), obj_hcls_guarded(false), preferred_dest_reg(-1) {
    }
};

struct GetattrRewriteArgs2 {
    Rewriter2 *rewriter;
    RewriterVarUsage2 obj;
    Location destination;
    bool more_guards_after;

    bool out_success;
    RewriterVarUsage2 out_rtn;

    bool obj_hcls_guarded;

    GetattrRewriteArgs2(Rewriter2 *rewriter, RewriterVarUsage2 &&obj, Location destination, bool more_guards_after) :
            rewriter(rewriter), obj(std::move(obj)), destination(destination), more_guards_after(more_guards_after), out_success(false), out_rtn(RewriterVarUsage2::empty()), obj_hcls_guarded(false) {
    }
};

struct SetattrRewriteArgs {
    Rewriter *rewriter;
    RewriterVar obj, attrval;
    bool out_success;

    SetattrRewriteArgs(Rewriter *rewriter, const RewriterVar &obj, const RewriterVar &attrval) :
            rewriter(rewriter), obj(obj), attrval(attrval), out_success(false) {
    }
};

struct SetattrRewriteArgs2 {
    Rewriter2 *rewriter;
    RewriterVarUsage2 obj, attrval;
    bool more_guards_after;

    bool out_success;

    SetattrRewriteArgs2(Rewriter2 *rewriter, RewriterVarUsage2 &&obj, RewriterVarUsage2 &&attrval, bool more_guards_after) :
            rewriter(rewriter), obj(std::move(obj)), attrval(std::move(attrval)), more_guards_after(more_guards_after), out_success(false) {
    }
};

struct LenRewriteArgs {
    Rewriter *rewriter;
    RewriterVar obj;
    bool out_success;
    RewriterVar out_rtn;

    int preferred_dest_reg;

    LenRewriteArgs(Rewriter *rewriter, const RewriterVar &obj) :
            rewriter(rewriter), obj(obj), out_success(false), preferred_dest_reg(-1) {
    }
};

struct CallRewriteArgs {
    Rewriter *rewriter;
    RewriterVar obj;
    RewriterVar arg1, arg2, arg3, args;
    bool out_success;
    RewriterVar out_rtn;
    bool func_guarded;
    bool args_guarded;

    int preferred_dest_reg;

    CallRewriteArgs(Rewriter *rewriter, const RewriterVar &obj) :
            rewriter(rewriter), obj(obj), out_success(false), func_guarded(false), args_guarded(false), preferred_dest_reg(-1) {
    }
};

struct BinopRewriteArgs {
    Rewriter *rewriter;
    RewriterVar lhs, rhs;
    bool out_success;
    RewriterVar out_rtn;

    BinopRewriteArgs(Rewriter *rewriter, const RewriterVar &lhs,
            const RewriterVar &rhs) : rewriter(rewriter), lhs(lhs), rhs(rhs), out_success(false) {
    }
};

struct CompareRewriteArgs {
    Rewriter *rewriter;
    RewriterVar lhs, rhs;
    bool out_success;
    RewriterVar out_rtn;

    CompareRewriteArgs(Rewriter *rewriter, const RewriterVar &lhs,
            const RewriterVar &rhs) : rewriter(rewriter), lhs(lhs), rhs(rhs), out_success(false) {
    }
};

Box* runtimeCallInternal(Box* obj, CallRewriteArgs *rewrite_args, int64_t nargs, Box* arg1, Box* arg2, Box* arg3, Box* *args);
static Box* (*runtimeCallInternal0)(Box*, CallRewriteArgs*, int64_t) = (Box* (*)(Box*, CallRewriteArgs*, int64_t))runtimeCallInternal;
static Box* (*runtimeCallInternal1)(Box*, CallRewriteArgs*, int64_t, Box*) = (Box* (*)(Box*, CallRewriteArgs*, int64_t, Box*))runtimeCallInternal;
static Box* (*runtimeCallInternal2)(Box*, CallRewriteArgs*, int64_t, Box*, Box*) = (Box* (*)(Box*, CallRewriteArgs*, int64_t, Box*, Box*))runtimeCallInternal;
static Box* (*runtimeCallInternal3)(Box*, CallRewriteArgs*, int64_t, Box*, Box*, Box*) = (Box* (*)(Box*, CallRewriteArgs*, int64_t, Box*, Box*, Box*))runtimeCallInternal;

Box* typeCallInternal(CallRewriteArgs *rewrite_args, int64_t nargs, Box* obj, Box* arg1, Box* arg2, Box** args);
static Box* (*typeCallInternal1)(CallRewriteArgs *rewrite_args, int64_t nargs, Box*) = (Box* (*)(CallRewriteArgs*, int64_t, Box*))typeCallInternal;
static Box* (*typeCallInternal2)(CallRewriteArgs *rewrite_args, int64_t nargs, Box*, Box*) = (Box* (*)(CallRewriteArgs*, int64_t, Box*, Box*))typeCallInternal;
static Box* (*typeCallInternal3)(CallRewriteArgs *rewrite_args, int64_t nargs, Box*, Box*, Box*) = (Box* (*)(CallRewriteArgs*, int64_t, Box*, Box*, Box*))typeCallInternal;

enum LookupScope {
    CLASS_ONLY = 1,
    INST_ONLY = 2,
    CLASS_OR_INST = 3,
};
bool checkClass(LookupScope scope) {
    return (scope & CLASS_ONLY) != 0;
}
bool checkInst(LookupScope scope) {
    return (scope & INST_ONLY) != 0;
}
extern "C" Box* callattrInternal(Box* obj, const std::string *attr, LookupScope, CallRewriteArgs *rewrite_args, int64_t nargs, Box* arg1, Box* arg2, Box* arg3, Box **args);
static Box* (*callattrInternal0)(Box*, const std::string*, LookupScope, CallRewriteArgs*, int64_t) = (Box* (*)(Box*, const std::string*, LookupScope, CallRewriteArgs*, int64_t))callattrInternal;
static Box* (*callattrInternal1)(Box*, const std::string*, LookupScope, CallRewriteArgs*, int64_t, Box*) = (Box* (*)(Box*, const std::string*, LookupScope, CallRewriteArgs*, int64_t, Box*))callattrInternal;
static Box* (*callattrInternal2)(Box*, const std::string*, LookupScope, CallRewriteArgs*, int64_t, Box*, Box*) = (Box* (*)(Box*, const std::string*, LookupScope, CallRewriteArgs*, int64_t, Box*, Box*))callattrInternal;
static Box* (*callattrInternal3)(Box*, const std::string*, LookupScope, CallRewriteArgs*, int64_t, Box*, Box*, Box*) = (Box* (*)(Box*, const std::string*, LookupScope, CallRewriteArgs*, int64_t, Box*, Box*, Box*))callattrInternal;

size_t PyHasher::operator() (Box* b) const {
    if (b->cls == str_cls) {
        std::hash<std::string> H;
        return H(static_cast<BoxedString*>(b)->s);
    }

    BoxedInt *i = hash(b);
    assert(sizeof(size_t) == sizeof(i->n));
    size_t rtn = i->n;
    return rtn;
}

bool PyEq::operator() (Box* lhs, Box* rhs) const {
    if (lhs->cls == rhs->cls) {
        if (lhs->cls == str_cls) {
            return static_cast<BoxedString*>(lhs)->s == static_cast<BoxedString*>(rhs)->s;
        }
    }

    // TODO fix this
    Box* cmp = compareInternal(lhs, rhs, AST_TYPE::Eq, NULL);
    assert(cmp->cls == bool_cls);
    BoxedBool* b = static_cast<BoxedBool*>(cmp);
    bool rtn = b->b;
    return rtn;
}

bool PyLt::operator() (Box* lhs, Box* rhs) const {
    // TODO fix this
    Box* cmp = compareInternal(lhs, rhs, AST_TYPE::Lt, NULL);
    assert(cmp->cls == bool_cls);
    BoxedBool* b = static_cast<BoxedBool*>(cmp);
    bool rtn = b->b;
    return rtn;
}

extern "C" void my_assert(bool b) {
    assert(b);
}

extern "C" void assertNameDefined(bool b, const char* name) {
    if (!b) {
        fprintf(stderr, "UnboundLocalError: local variable '%s' referenced before assignment\n", name);
        raiseExc();
    }
}

extern "C" void raiseAttributeErrorStr(const char* typeName, const char* attr) {
    fprintf(stderr, "AttributeError: '%s' object has no attribute '%s'\n", typeName, attr);
    raiseExc();
}

extern "C" void raiseAttributeError(Box* obj, const char* attr) {
    if (obj->cls == type_cls) {
        fprintf(stderr, "AttributeError: type object '%s' has no attribute '%s'\n", getNameOfClass(static_cast<BoxedClass*>(obj))->c_str(), attr);
    } else {
        raiseAttributeErrorStr(getTypeName(obj)->c_str(), attr);
    }
    raiseExc();
}

extern "C" void raiseNotIterableError(const char* typeName) {
    fprintf(stderr, "TypeError: '%s' object is not iterable\n", typeName);
    raiseExc();
}

extern "C" void checkUnpackingLength(i64 expected, i64 given) {
    if (given == expected)
        return;

    if (given > expected)
        fprintf(stderr, "ValueError: too many values to unpack\n");
    else {
        if (given == 1)
            fprintf(stderr, "ValueError: need more than %ld value to unpack\n", given);
        else
            fprintf(stderr, "ValueError: need more than %ld values to unpack\n", given);
    }
    raiseExc();
}

BoxedClass::BoxedClass(bool hasattrs, BoxedClass::Dtor dtor): HCBox(&type_flavor, type_cls), hasattrs(hasattrs), dtor(dtor), is_constant(false) {
}

extern "C" const std::string* getNameOfClass(BoxedClass* cls) {
    Box* b = cls->peekattr("__name__");
    assert(b);
    ASSERT(b->cls == str_cls, "%p", b->cls);
    BoxedString* sb = static_cast<BoxedString*>(b);
    return &sb->s;
}

extern "C" const std::string* getTypeName(Box* o) {
    return getNameOfClass(o->cls);
}

HiddenClass* HiddenClass::getOrMakeChild(const std::string& attr) {
    std::unordered_map<std::string, HiddenClass*>::iterator it = children.find(attr);
    if (it != children.end())
        return it->second;

    static StatCounter num_hclses("num_hidden_classes");
    num_hclses.log();

    HiddenClass* rtn = new HiddenClass(this);
    this->children[attr] = rtn;
    rtn->attr_offsets[attr] = attr_offsets.size();
    return rtn;
}

HiddenClass* HiddenClass::getRoot() {
    static HiddenClass* root = new HiddenClass();
    return root;
}

HCBox::HCBox(const ObjectFlavor *flavor, BoxedClass *cls) : Box(flavor, cls), hcls(HiddenClass::getRoot()), attr_list(NULL) {
    assert(!cls || flavor->isUserDefined() == isUserDefined(cls));

    // first part of this check is to handle if we're building "type" itself, since when
    // it's constructed its type (which should be "type") is NULL.
    assert((cls == NULL && type_cls == NULL) || cls->hasattrs);
}


Box* HCBox::getattr(const std::string &attr, GetattrRewriteArgs* rewrite_args, GetattrRewriteArgs2* rewrite_args2) {
    if (rewrite_args) {
        rewrite_args->out_success = true;

        if (!rewrite_args->obj_hcls_guarded)
            rewrite_args->obj.addAttrGuard(BOX_HCLS_OFFSET, (intptr_t)this->hcls);
    }

    if (rewrite_args2) {
        rewrite_args2->out_success = true;

        if (!rewrite_args2->obj_hcls_guarded)
            rewrite_args2->obj.addAttrGuard(BOX_HCLS_OFFSET, (intptr_t)this->hcls);
    }

    int offset = hcls->getOffset(attr);
    if (offset == -1)
        return NULL;

    if (rewrite_args) {
        // TODO using the output register as the temporary makes register allocation easier
        // since we don't need to clobber a register, but does it make the code slower?
        //int temp_reg = -2;
        //if (rewrite_args->preferred_dest_reg == -2)
            //temp_reg = -3;
        int temp_reg = rewrite_args->preferred_dest_reg;

        RewriterVar attrs = rewrite_args->obj.getAttr(BOX_ATTRS_OFFSET, temp_reg);
        rewrite_args->out_rtn = attrs.getAttr(offset * sizeof(Box*) + ATTRLIST_ATTRS_OFFSET, rewrite_args->preferred_dest_reg);

        rewrite_args->rewriter->addDependenceOn(cls->dependent_icgetattrs);
    }

    if (rewrite_args2) {
        if (!rewrite_args2->more_guards_after)
            rewrite_args2->rewriter->setDoneGuarding();

        RewriterVarUsage2 attrs = rewrite_args2->obj.getAttr(BOX_ATTRS_OFFSET, RewriterVarUsage2::Kill);
        rewrite_args2->out_rtn = attrs.getAttr(offset * sizeof(Box*) + ATTRLIST_ATTRS_OFFSET, RewriterVarUsage2::Kill, rewrite_args2->destination);
    }

    Box* rtn = attr_list->attrs[offset];
    return rtn;
}

void HCBox::giveAttr(const std::string& attr, Box* val) {
    assert(this->peekattr(attr) == NULL);
    this->setattr(attr, val, NULL, NULL);
}

void HCBox::setattr(const std::string& attr, Box* val, SetattrRewriteArgs *rewrite_args, SetattrRewriteArgs2 *rewrite_args2) {
    RELEASE_ASSERT(attr != "None" || this == builtins_module, "can't assign to None");

    bool isgetattr = (attr == "__getattr__" || attr == "__getattribute__");
    if (isgetattr && this->cls == type_cls) {
        // Will have to embed the clear in the IC, so just disable the patching for now:
        rewrite_args = NULL;
        rewrite_args2 = NULL;

        // TODO should put this clearing behavior somewhere else, since there are probably more
        // cases in which we want to do it.
        BoxedClass *self = static_cast<BoxedClass*>(this);
        self->dependent_icgetattrs.invalidateAll();
    }

    HiddenClass *hcls = this->hcls;
    int numattrs = hcls->attr_offsets.size();

    int offset = hcls->getOffset(attr);

    if (rewrite_args) {
        rewrite_args->obj.addAttrGuard(BOX_HCLS_OFFSET, (intptr_t)hcls);
        rewrite_args->rewriter->addDecision(offset == -1 ? 1 : 0);
    }

    if (rewrite_args2) {
        rewrite_args2->obj.addAttrGuard(BOX_HCLS_OFFSET, (intptr_t)hcls);

        if (!rewrite_args2->more_guards_after)
            rewrite_args2->rewriter->setDoneGuarding();
        //rewrite_args2->rewriter->addDecision(offset == -1 ? 1 : 0);
    }

    if (offset >= 0) {
        assert(offset < numattrs);
        Box* prev = this->attr_list->attrs[offset];
        this->attr_list->attrs[offset] = val;

        if (rewrite_args) {
            RewriterVar r_hattrs = rewrite_args->obj.getAttr(BOX_ATTRS_OFFSET, 1);

            r_hattrs.setAttr(offset * sizeof(Box*) + ATTRLIST_ATTRS_OFFSET, rewrite_args->attrval);
            rewrite_args->out_success = true;
        }

        if (rewrite_args2) {

            RewriterVarUsage2 r_hattrs = rewrite_args2->obj.getAttr(BOX_ATTRS_OFFSET, RewriterVarUsage2::Kill, Location::any());

            r_hattrs.setAttr(offset * sizeof(Box*) + ATTRLIST_ATTRS_OFFSET, std::move(rewrite_args2->attrval));
            r_hattrs.setDoneUsing();

            rewrite_args2->out_success = true;
        }

        return;
    }

    assert(offset == -1);
    HiddenClass *new_hcls = hcls->getOrMakeChild(attr);

    // TODO need to make sure we don't need to rearrange the attributes
    assert(new_hcls->attr_offsets[attr] == numattrs);
#ifndef NDEBUG
    for (const auto &p : hcls->attr_offsets) {
        assert(new_hcls->attr_offsets[p.first] == p.second);
    }
#endif

    if (rewrite_args) {
        rewrite_args->obj.push();
        rewrite_args->attrval.push();
    }



    RewriterVar r_new_array;
    RewriterVarUsage2 r_new_array2(RewriterVarUsage2::empty());
    int new_size = sizeof(HCBox::AttrList) + sizeof(Box*) * (numattrs + 1);
    if (numattrs == 0) {
        this->attr_list = (HCBox::AttrList*)rt_alloc(new_size);
        this->attr_list->gc_header.kind_id = untracked_kind.kind_id;
        if (rewrite_args) {
            rewrite_args->rewriter->loadConst(0, new_size);
            r_new_array = rewrite_args->rewriter->call((void*)rt_alloc);
            RewriterVar r_flavor = rewrite_args->rewriter->loadConst(0, (intptr_t)untracked_kind.kind_id);
            r_new_array.setAttr(ATTRLIST_KIND_OFFSET, r_flavor);
        }
        if (rewrite_args2) {
            RewriterVarUsage2 r_newsize = rewrite_args2->rewriter->loadConst(new_size, Location::forArg(0));
            r_new_array2 = rewrite_args2->rewriter->call(false, (void*)rt_alloc, std::move(r_newsize));
            RewriterVarUsage2 r_flavor = rewrite_args2->rewriter->loadConst((int64_t)untracked_kind.kind_id);
            r_new_array2.setAttr(ATTRLIST_KIND_OFFSET, std::move(r_flavor));
        }
    } else {
        this->attr_list = (HCBox::AttrList*)rt_realloc(this->attr_list, new_size);
        if (rewrite_args) {
            rewrite_args->obj.getAttr(BOX_ATTRS_OFFSET, 0);
            rewrite_args->rewriter->loadConst(1, new_size);
            r_new_array = rewrite_args->rewriter->call((void*)rt_realloc);
        }
        if (rewrite_args2) {
            RewriterVarUsage2 r_oldarray = rewrite_args2->obj.getAttr(BOX_ATTRS_OFFSET, RewriterVarUsage2::NoKill, Location::forArg(0));
            RewriterVarUsage2 r_newsize = rewrite_args2->rewriter->loadConst(new_size, Location::forArg(1));
            r_new_array2 = rewrite_args2->rewriter->call(false, (void*)rt_realloc, std::move(r_oldarray), std::move(r_newsize));
        }
    }
    // Don't set the new hcls until after we do the allocation for the new attr_list;
    // that allocation can cause a collection, and we want the collector to always
    // see a consistent state between the hcls and the attr_list
    this->hcls = new_hcls;

    if (rewrite_args) {
        RewriterVar attrval = rewrite_args->rewriter->pop(0);
        RewriterVar obj = rewrite_args->rewriter->pop(2);
        obj.setAttr(BOX_ATTRS_OFFSET, r_new_array);
        r_new_array.setAttr(numattrs * sizeof(Box*) + ATTRLIST_ATTRS_OFFSET, attrval);
        RewriterVar hcls = rewrite_args->rewriter->loadConst(1, (intptr_t)new_hcls);
        obj.setAttr(BOX_HCLS_OFFSET, hcls);
        rewrite_args->out_success = true;
    }
    if (rewrite_args2) {
        r_new_array2.setAttr(numattrs * sizeof(Box*) + ATTRLIST_ATTRS_OFFSET, std::move(rewrite_args2->attrval));
        rewrite_args2->obj.setAttr(BOX_ATTRS_OFFSET, std::move(r_new_array2));

        RewriterVarUsage2 r_hcls = rewrite_args2->rewriter->loadConst((intptr_t)new_hcls);
        rewrite_args2->obj.setAttr(BOX_HCLS_OFFSET, std::move(r_hcls));
        rewrite_args2->obj.setDoneUsing();

        rewrite_args2->out_success = true;
    }
    this->attr_list->attrs[numattrs] = val;
}

static Box* _handleClsAttr(Box* obj, Box* attr) {
    if (attr->cls == function_cls) {
        Box* rtn = boxInstanceMethod(obj, attr);
        return rtn;
    }
    return attr;
}

Box* getclsattr_internal(Box* obj, const char* attr, GetattrRewriteArgs *rewrite_args, GetattrRewriteArgs2 *rewrite_args2) {
    Box* val;

    if (rewrite_args) {
        //rewrite_args->rewriter->nop();
        //rewrite_args->rewriter->trap();
        RewriterVar cls = rewrite_args->obj.getAttr(BOX_CLS_OFFSET, 4);

        //rewrite_args->obj.push();
        GetattrRewriteArgs sub_rewrite_args(rewrite_args->rewriter, cls);
        sub_rewrite_args.preferred_dest_reg = 1;
        val = getattr_internal(obj->cls, attr, false, false, &sub_rewrite_args, NULL);
        //rewrite_args->obj = rewrite_args->rewriter->pop(0);

        if (!sub_rewrite_args.out_success) {
            rewrite_args = NULL;
        } else {
            if (val)
                rewrite_args->out_rtn = sub_rewrite_args.out_rtn;
        }
    } else if (rewrite_args2) {
        RewriterVarUsage2 cls = rewrite_args2->obj.getAttr(BOX_CLS_OFFSET, RewriterVarUsage2::NoKill);

        GetattrRewriteArgs2 sub_rewrite_args(rewrite_args2->rewriter, std::move(cls), Location::forArg(1), rewrite_args2->more_guards_after);
        val = getattr_internal(obj->cls, attr, false, false, NULL, &sub_rewrite_args);

        if (!sub_rewrite_args.out_success) {
            sub_rewrite_args.obj.setDoneUsing();
            rewrite_args2 = NULL;
        } else {
            if (val) {
                rewrite_args2->out_rtn = std::move(sub_rewrite_args.out_rtn);
            } else {
                sub_rewrite_args.obj.setDoneUsing();
            }
        }
    } else {
        val = getattr_internal(obj->cls, attr, false, false, NULL, NULL);
    }

    if (val == NULL) {
        if (rewrite_args) rewrite_args->out_success = true;
        if (rewrite_args2) rewrite_args2->out_success = true;
        return val;
    }

    if (rewrite_args) {
        //rewrite_args->rewriter->trap();
        rewrite_args->obj = rewrite_args->obj.move(0);
        RewriterVar val = rewrite_args->out_rtn.move(1);

        // TODO could speculate that the attr type is the same, ie guard on the attr type
        // and then either always create the IM or never
        RewriterVar rrtn = rewrite_args->rewriter->call((void*)_handleClsAttr);
        rewrite_args->out_rtn = rrtn;
        rewrite_args->out_success = true;
    }

    if (rewrite_args2) {
        // Ok this is a lie, _handleClsAttr can call back into python because it does GC collection.
        // I guess it should disable GC or something...
        RewriterVarUsage2 rrtn = rewrite_args2->rewriter->call(false, (void*)_handleClsAttr, std::move(rewrite_args2->obj), std::move(rewrite_args2->out_rtn));
        rewrite_args2->out_rtn = std::move(rrtn);
        rewrite_args2->out_success = true;
    }

    return _handleClsAttr(obj, val);
}

extern "C" Box* getclsattr(Box* obj, const char* attr) {
    static StatCounter slowpath_getclsattr("slowpath_getclsattr");
    slowpath_getclsattr.log();

    Box* gotten;

#if 0
    std::unique_ptr<Rewriter> rewriter(Rewriter::createRewriter(__builtin_extract_return_addr(__builtin_return_address(0)), 2, 1, "getclsattr"));

    if (rewriter.get()) {
        //rewriter->trap();
        GetattrRewriteArgs rewrite_args(rewriter.get(), rewriter->getArg(0));
        gotten = getclsattr_internal(obj, attr, &rewrite_args, NULL);

        if (rewrite_args.out_success && gotten) {
            rewrite_args.out_rtn.move(-1);
            rewriter->commit();
        }
#else
    std::unique_ptr<Rewriter2> rewriter(Rewriter2::createRewriter(__builtin_extract_return_addr(__builtin_return_address(0)), 2, "getclsattr"));

    if (rewriter.get()) {
        //rewriter->trap();
        GetattrRewriteArgs2 rewrite_args(rewriter.get(), rewriter->getArg(0), rewriter->getReturnDestination(), false);
        gotten = getclsattr_internal(obj, attr, NULL, &rewrite_args);

        if (rewrite_args.out_success && gotten) {
            rewriter->commitReturning(std::move(rewrite_args.out_rtn));
        }
#endif
    } else {
        gotten = getclsattr_internal(obj, attr, NULL, NULL);
    }
    RELEASE_ASSERT(gotten, "%s:%s", getTypeName(obj)->c_str(), attr);

    return gotten;
}

static Box* (*runtimeCall0)(Box*, int64_t) = (Box* (*)(Box*, int64_t))runtimeCall;
static Box* (*runtimeCall1)(Box*, int64_t, Box*) = (Box* (*)(Box*, int64_t, Box*))runtimeCall;
static Box* (*runtimeCall2)(Box*, int64_t, Box*, Box*) = (Box* (*)(Box*, int64_t, Box*, Box*))runtimeCall;
static Box* (*runtimeCall3)(Box*, int64_t, Box*, Box*, Box*) = (Box* (*)(Box*, int64_t, Box*, Box*, Box*))runtimeCall;

Box* getattr_internal(Box *obj, const char* attr, bool check_cls, bool allow_custom, GetattrRewriteArgs* rewrite_args, GetattrRewriteArgs2* rewrite_args2) {
    if (allow_custom) {
        // Don't need to pass icentry args, since we special-case __getattribtue__ and __getattr__ to use
        // invalidation rather than guards
        Box* getattribute = getclsattr_internal(obj, "__getattribute__", NULL, NULL);
        if (getattribute) {
            // TODO this is a good candidate for interning?
            Box* boxstr = boxStrConstant(attr);
            Box* rtn = runtimeCall1(getattribute, 1, boxstr);
            return rtn;
        }

        if (rewrite_args) {
            rewrite_args->rewriter->addDependenceOn(obj->cls->dependent_icgetattrs);
        }
        if (rewrite_args2) {
            rewrite_args2->rewriter->addDependenceOn(obj->cls->dependent_icgetattrs);
        }
    }

    if (obj->cls->hasattrs) {
        HCBox* hobj = static_cast<HCBox*>(obj);

        Box* val = NULL;
        if (rewrite_args) {
            GetattrRewriteArgs hrewrite_args(rewrite_args->rewriter, rewrite_args->obj);
            hrewrite_args.preferred_dest_reg = rewrite_args->preferred_dest_reg;
            val = hobj->getattr(attr, &hrewrite_args, NULL);

            if (hrewrite_args.out_success) {
                if (val)
                    rewrite_args->out_rtn = hrewrite_args.out_rtn;
            } else {
                rewrite_args = NULL;
            }
        } else if (rewrite_args2) {
            GetattrRewriteArgs2 hrewrite_args(rewrite_args2->rewriter, std::move(rewrite_args2->obj), rewrite_args2->destination, rewrite_args2->more_guards_after);
            val = hobj->getattr(attr, NULL, &hrewrite_args);

            if (hrewrite_args.out_success) {
                if (val)
                    rewrite_args2->out_rtn = std::move(hrewrite_args.out_rtn);
                else
                    rewrite_args2->obj = std::move(hrewrite_args.obj);
            } else {
                rewrite_args2 = NULL;
            }
        } else {
            val = hobj->getattr(attr, NULL, NULL);
        }

        if (val) {
            if (rewrite_args) rewrite_args->out_success = true;
            if (rewrite_args2) rewrite_args2->out_success = true;
            return val;
        }
    }

    if (allow_custom) {
        // Don't need to pass icentry args, since we special-case __getattribtue__ and __getattr__ to use
        // invalidation rather than guards
        Box* getattr = getclsattr_internal(obj, "__getattr__", NULL, NULL);
        if (getattr) {
            Box* boxstr = boxStrConstant(attr);
            Box* rtn = runtimeCall1(getattr, 1, boxstr);
            return rtn;
        }

        if (rewrite_args) {
            rewrite_args->rewriter->addDependenceOn(obj->cls->dependent_icgetattrs);
        }
        if (rewrite_args2) {
            rewrite_args2->rewriter->addDependenceOn(obj->cls->dependent_icgetattrs);
        }
    }

    Box *rtn = NULL;
    if (check_cls) {
        if (rewrite_args) {
            GetattrRewriteArgs crewrite_args(rewrite_args->rewriter, rewrite_args->obj);
            rtn = getclsattr_internal(obj, attr, &crewrite_args, NULL);

            if (!crewrite_args.out_success) {
                rewrite_args = NULL;
            } else {
                if (rtn)
                    rewrite_args->out_rtn = crewrite_args.out_rtn;
            }
        } else if (rewrite_args2) {
            GetattrRewriteArgs2 crewrite_args(rewrite_args2->rewriter, std::move(rewrite_args2->obj), rewrite_args2->destination, rewrite_args2->more_guards_after);
            rtn = getclsattr_internal(obj, attr, NULL, &crewrite_args);

            if (!crewrite_args.out_success) {
                rewrite_args2 = NULL;
            } else {
                if (rtn)
                    rewrite_args2->out_rtn = std::move(crewrite_args.out_rtn);
                else
                    rewrite_args2->obj = std::move(crewrite_args.obj);
            }
        } else {
            rtn = getclsattr_internal(obj, attr, NULL, NULL);
        }
    }
    if (rewrite_args) rewrite_args->out_success = true;
    if (rewrite_args2) rewrite_args2->out_success = true;

    return rtn;
}

extern "C" Box* getattr(Box* obj, const char* attr) {
    static StatCounter slowpath_getattr("slowpath_getattr");
    slowpath_getattr.log();

    if (VERBOSITY() >= 2) {
        std::string per_name_stat_name = "getattr__" + std::string(attr);
        int id = Stats::getStatId(per_name_stat_name);
        Stats::log(id);
    }

    { /* anonymous scope to make sure destructors get run before we err out */
#if 1
        std::unique_ptr<Rewriter2> rewriter(Rewriter2::createRewriter(__builtin_extract_return_addr(__builtin_return_address(0)), 2, "getattr"));

        Box* val;
        if (rewriter.get()) {
            //rewriter->trap();
            Location dest;
            TypeRecorder* recorder = rewriter->getTypeRecorder();
            if (recorder)
                dest = Location::forArg(1);
            else
                dest = rewriter->getReturnDestination();
            GetattrRewriteArgs2 rewrite_args(rewriter.get(), rewriter->getArg(0), dest, false);
            val = getattr_internal(obj, attr, 1, true, NULL, &rewrite_args);

            if (rewrite_args.out_success && val) {
                if (recorder) {
                    RewriterVarUsage2 record_rtn = rewriter->call(false, (void*)recordType, rewriter->loadConst((intptr_t)recorder, Location::forArg(0)), std::move(rewrite_args.out_rtn));
                    rewriter->commitReturning(std::move(record_rtn));

                    recordType(recorder, val);
                } else {
                    rewriter->commitReturning(std::move(rewrite_args.out_rtn));
                }
            } else {
                rewrite_args.obj.setDoneUsing();
            }
#else

        std::unique_ptr<Rewriter> rewriter(Rewriter::createRewriter(__builtin_extract_return_addr(__builtin_return_address(0)), 2, 1, "getattr"));

        Box* val;
        if (rewriter.get()) {
            //rewriter->trap();
            GetattrRewriteArgs rewrite_args(rewriter.get(), rewriter->getArg(0));
            val = getattr_internal(obj, attr, 1, true, &rewrite_args, NULL);

            if (rewrite_args.out_success && val) {
                rewrite_args.out_rtn.move(-1);
                rewriter->commit();
            }
#endif
        } else {
            val = getattr_internal(obj, attr, 1, true, NULL, NULL);
        }

        if (val) {
            return val;
        }
    }

    raiseAttributeError(obj, attr);
}

extern "C" void setattr(Box *obj, const char* attr, Box* attr_val) {
    assert(strcmp(attr, "__class__") != 0);

    static StatCounter slowpath_setattr("slowpath_setattr");
    slowpath_setattr.log();

    if (!obj->cls->hasattrs) {
        raiseAttributeError(obj, attr);
    }

    if (obj->cls == type_cls) {
        BoxedClass* cobj = static_cast<BoxedClass*>(obj);
        if (!isUserDefined(cobj)) {
            fprintf(stderr, "TypeError: can't set attributes of built-in/extension type '%s'\n", getNameOfClass(cobj)->c_str());
            raiseExc();
        }
    }

    HCBox* hobj = static_cast<HCBox*>(obj);

#if 0
    std::unique_ptr<Rewriter> rewriter(Rewriter::createRewriter(__builtin_extract_return_addr(__builtin_return_address(0)), 3, 1, "setattr"));

    if (rewriter.get()) {
        //rewriter->trap();
        SetattrRewriteArgs rewrite_args(rewriter.get(), rewriter->getArg(0), rewriter->getArg(2));
        hobj->setattr(attr, attr_val, &rewrite_args);
        if (rewrite_args.out_success) {
            rewriter->commit();
        }
#else
    std::unique_ptr<Rewriter2> rewriter(Rewriter2::createRewriter(__builtin_extract_return_addr(__builtin_return_address(0)), 3, "setattr"));

    if (rewriter.get()) {
        //rewriter->trap();
        SetattrRewriteArgs2 rewrite_args(rewriter.get(), rewriter->getArg(0), rewriter->getArg(2), false);
        hobj->setattr(attr, attr_val, NULL, &rewrite_args);
        if (rewrite_args.out_success) {
            rewriter->commit();
        } else {
            rewrite_args.obj.setDoneUsing();
            rewrite_args.attrval.setDoneUsing();
        }
#endif
    } else {
        hobj->setattr(attr, attr_val, NULL, NULL);
    }
}

bool isUserDefined(BoxedClass *cls) {
    // TODO I don't think this is good enough?
    return cls->hasattrs && (cls != function_cls && cls != type_cls) && !cls->is_constant;
}

extern "C" bool nonzero(Box* obj) {
    static StatCounter slowpath_nonzero("slowpath_nonzero");

    std::unique_ptr<Rewriter> rewriter(Rewriter::createRewriter(__builtin_extract_return_addr(__builtin_return_address(0)), 1, 0, "nonzero"));

    if (rewriter.get()) {
        //rewriter->trap();
        rewriter->getArg(0).addAttrGuard(BOX_CLS_OFFSET, (intptr_t)obj->cls);
    }

    if (obj->cls == bool_cls) {
        if (rewriter.get()) {
            rewriter->getArg(0).getAttr(BOOL_B_OFFSET, -1);
            rewriter->commit();
        }

        BoxedBool *bool_obj = static_cast<BoxedBool*>(obj);
        return bool_obj->b;
    } else if (obj->cls == int_cls) {
        if (rewriter.get()) {
            // TODO should do:
            // test 	%rsi, %rsi
            // setne	%al
            RewriterVar n = rewriter->getArg(0).getAttr(INT_N_OFFSET, 1);
            n.toBool(-1);
            rewriter->commit();
        }

        BoxedInt *int_obj = static_cast<BoxedInt*>(obj);
        return int_obj->n != 0;
    } else if (obj->cls == float_cls) {
        if (rewriter.get()) {
            rewriter->call((void*)floatNonzeroUnboxed);
            rewriter->commit();
        }
        return static_cast<BoxedFloat*>(obj)->d != 0;
    }

    // FIXME we have internal functions calling this method;
    // instead, we should break this out into an external and internal function.
    // slowpath_* counters are supposed to count external calls; putting it down
    // here gets a better representation of that.
    // TODO move internal callers to nonzeroInternal, and log *all* calls to nonzero
    slowpath_nonzero.log();

    //int id = Stats::getStatId("slowpath_nonzero_" + *getTypeName(obj));
    //Stats::log(id);

    Box* func = getclsattr_internal(obj, "__nonzero__", NULL, NULL);
    if (func == NULL) {
        RELEASE_ASSERT(isUserDefined(obj->cls), "%s.__nonzero__", getTypeName(obj)->c_str()); // TODO
        return true;
    }

    Box* r = runtimeCall0(func, 0);
    if (r->cls == bool_cls) {
        BoxedBool* b = static_cast<BoxedBool*>(r);
        bool rtn = b->b;
        return rtn;
    } else if (r->cls == int_cls) {
        BoxedInt* b = static_cast<BoxedInt*>(r);
        bool rtn = b->n != 0;
        return rtn;
    } else {
        fprintf(stderr, "TypeError: __nonzero__ should return bool or int, returned %s\n", getTypeName(r)->c_str());
        raiseExc();
    }
}

extern "C" BoxedString* str(Box* obj) {
    static StatCounter slowpath_str("slowpath_str");
    slowpath_str.log();

    if (obj->cls != str_cls) {
        Box *str = getclsattr_internal(obj, "__str__", NULL, NULL);
        if (str == NULL)
            str = getclsattr_internal(obj, "__repr__", NULL, NULL);

        if (str == NULL) {
            ASSERT(isUserDefined(obj->cls), "%s.__str__", getTypeName(obj)->c_str());

            char buf[80];
            snprintf(buf, 80, "<%s object at %p>", getTypeName(obj)->c_str(), obj);
            return boxStrConstant(buf);
        } else {
            obj = runtimeCallInternal0(str, NULL, 0);
        }
    }
    if (obj->cls != str_cls) {
        fprintf(stderr, "__str__ did not return a string!\n");
        abort();
    }
    return static_cast<BoxedString*>(obj);
}

extern "C" BoxedString* repr(Box* obj) {
    static StatCounter slowpath_repr("slowpath_repr");
    slowpath_repr.log();

    Box *repr = getclsattr_internal(obj, "__repr__", NULL, NULL);
    if (repr == NULL) {
        ASSERT(isUserDefined(obj->cls), "%s", getTypeName(obj)->c_str());

        char buf[80];
        if (obj->cls == type_cls) {
            snprintf(buf, 80, "<type '%s'>", getNameOfClass(static_cast<BoxedClass*>(obj))->c_str());
        } else {
            snprintf(buf, 80, "<%s object at %p>", getTypeName(obj)->c_str(), obj);
        }
        return boxStrConstant(buf);
    } else {
        obj = runtimeCall0(repr, 0);
    }

    if (obj->cls != str_cls) {
        fprintf(stderr, "__repr__ did not return a string!\n");
        raiseExc();
    }
    return static_cast<BoxedString*>(obj);
}

extern "C" BoxedInt* hash(Box* obj) {
    static StatCounter slowpath_hash("slowpath_hash");
    slowpath_hash.log();

    Box *hash = getclsattr_internal(obj, "__hash__", NULL, NULL);
    if (hash == NULL) {
        ASSERT(isUserDefined(obj->cls), "%s.__hash__", getTypeName(obj)->c_str());
        // TODO not the best way to handle this...
        return static_cast<BoxedInt*>(boxInt((i64)obj));
    }

    Box* rtn = runtimeCall0(hash, 0);
    if (rtn->cls != int_cls) {
        fprintf(stderr, "TypeError: an integer is required\n");
        raiseExc();
    }
    return static_cast<BoxedInt*>(rtn);
}

extern "C" BoxedInt* lenInternal(Box* obj, LenRewriteArgs *rewrite_args) {
    Box* rtn;
    static std::string attr_str("__len__");
    if (rewrite_args) {
        CallRewriteArgs crewrite_args(rewrite_args->rewriter, rewrite_args->obj);
        crewrite_args.preferred_dest_reg = rewrite_args->preferred_dest_reg;
        rtn = callattrInternal0(obj, &attr_str, CLASS_ONLY, &crewrite_args, 0);
        if (!crewrite_args.out_success)
            rewrite_args = NULL;
        else if (rtn)
            rewrite_args->out_rtn = crewrite_args.out_rtn;
    } else {
        rtn = callattrInternal0(obj, &attr_str, CLASS_ONLY, NULL, 0);
    }

    if (rtn == NULL) {
        fprintf(stderr, "TypeError: object of type '%s' has no len()\n", getTypeName(obj)->c_str());
        raiseExc();
    }

    if (rtn->cls != int_cls) {
        fprintf(stderr, "TypeError: an integer is required\n");
        raiseExc();
    }

    if (rewrite_args)
        rewrite_args->out_success = true;
    return static_cast<BoxedInt*>(rtn);
}

extern "C" BoxedInt* len(Box* obj) {
    static StatCounter slowpath_len("slowpath_len");
    slowpath_len.log();

    return lenInternal(obj, NULL);
}

extern "C" i64 unboxedLen(Box* obj) {
    static StatCounter slowpath_unboxedlen("slowpath_unboxedlen");
    slowpath_unboxedlen.log();

    std::unique_ptr<Rewriter> rewriter(Rewriter::createRewriter(__builtin_extract_return_addr(__builtin_return_address(0)), 1, 1, "unboxedLen"));

    BoxedInt* lobj;
    RewriterVar r_boxed;
    if (rewriter.get()) {
        //rewriter->trap();
        LenRewriteArgs rewrite_args(rewriter.get(), rewriter->getArg(0));
        rewrite_args.preferred_dest_reg = 0;
        lobj = lenInternal(obj, &rewrite_args);

        if (!rewrite_args.out_success)
            rewriter.reset(NULL);
        else
            r_boxed = rewrite_args.out_rtn;
    } else {
        lobj = lenInternal(obj, NULL);
    }

    assert(lobj->cls == int_cls);
    i64 rtn = lobj->n;

    if (rewriter.get()) {
        RewriterVar rtn = r_boxed.getAttr(INT_N_OFFSET, -1);
        rewriter->commit();
    }
    return rtn;
}

extern "C" void print(Box *obj) {
    static StatCounter slowpath_print("slowpath_print");
    slowpath_print.log();

    BoxedString *strd = str(obj);
    printf("%s", strd->s.c_str());
}

extern "C" void dump(Box *obj) {
    printf("dump: obj %p, cls %p\n", obj, obj->cls);
}

// For rewriting purposes, this function assumes that nargs will be constant.
// That's probably fine for some uses (ex binops), but otherwise it should be guarded on beforehand.
extern "C" Box* callattrInternal(Box* obj, const std::string *attr, LookupScope scope, CallRewriteArgs *rewrite_args, int64_t nargs, Box* arg1, Box* arg2, Box* arg3, Box **args) {
    if (rewrite_args) {
        //if (VERBOSITY()) {
            //printf("callattrInternal: %d", rewrite_args->obj.getArgnum());
            //if (nargs >= 1) printf(" %d", rewrite_args->arg1.getArgnum());
            //if (nargs >= 2) printf(" %d", rewrite_args->arg2.getArgnum());
            //if (nargs >= 3) printf(" %d", rewrite_args->arg3.getArgnum());
            //if (nargs >= 4) printf(" %d", rewrite_args->args.getArgnum());
            //printf("\n");
        //}
        if (rewrite_args->obj.getArgnum() == -1) {
            //rewrite_args->rewriter->trap();
            rewrite_args->obj = rewrite_args->obj.move(-3);
        }
    }

    if (rewrite_args && !rewrite_args->args_guarded) {
        // TODO duplication with runtime_call
        // TODO should know which args don't need to be guarded, ex if we're guaranteed that they
        // already fit, either since the type inferencer could determine that,
        // or because they only need to fit into an UNKNOWN slot.

        if (nargs >= 1) rewrite_args->arg1.addAttrGuard(BOX_CLS_OFFSET, (intptr_t)arg1->cls);
        if (nargs >= 2) rewrite_args->arg2.addAttrGuard(BOX_CLS_OFFSET, (intptr_t)arg2->cls);
        // Have to move(-1) since the arg is (probably/maybe) on the stack;
        // TODO ideally would handle that case, but for now just do the move() which
        // it knows how to handle
        if (nargs >= 3) rewrite_args->arg3.move(-2).addAttrGuard(BOX_CLS_OFFSET, (intptr_t)arg3->cls);

        if (nargs > 3) {
            RewriterVar r_args = rewrite_args->args.move(-3);
            for (int i = 3; i < nargs; i++) {
                // TODO if there are a lot of args (>16), might be better to increment a pointer
                // rather index them directly?
                r_args.getAttr((i - 3) * sizeof(Box*), -2).addAttrGuard(BOX_CLS_OFFSET, (intptr_t)args[i-3]->cls);
            }
        }
    }


    if (checkInst(scope)) {
        Box* inst_attr;
        RewriterVar r_instattr;
        if (rewrite_args) {
            GetattrRewriteArgs ga_rewrite_args(rewrite_args->rewriter, rewrite_args->obj);

            inst_attr = getattr_internal(obj, attr->c_str(), false, true, &ga_rewrite_args, NULL);

            if (!ga_rewrite_args.out_success)
                rewrite_args = NULL;
            else if (inst_attr)
                r_instattr = ga_rewrite_args.out_rtn;
        } else {
            inst_attr = getattr_internal(obj, attr->c_str(), false, true, NULL, NULL);
        }

        if (inst_attr) {
            Box* rtn;
            if (inst_attr->cls != function_cls)
                rewrite_args = NULL;

            if (rewrite_args) {
                r_instattr.push();

                rewrite_args->args_guarded = true;

                r_instattr.addGuard((intptr_t)inst_attr);
                rewrite_args->func_guarded = true;

                rtn = runtimeCallInternal(inst_attr, rewrite_args, nargs, arg1, arg2, arg3, args);

                if (rewrite_args->out_success) {
                    r_instattr = rewrite_args->rewriter->pop(0);
                }
            } else {
                rtn = runtimeCallInternal(inst_attr, NULL, nargs, arg1, arg2, arg3, args);
            }

            if (!rtn) {
                fprintf(stderr, "TypeError: '%s' object is not callable\n", getTypeName(inst_attr)->c_str());
                raiseExc();
            }

            return rtn;
        }
    }

    Box* clsattr = NULL;
    RewriterVar r_clsattr;
    if (checkClass(scope)) {
        if (rewrite_args) {
            //rewrite_args->obj.push();
            RewriterVar r_cls = rewrite_args->obj.getAttr(BOX_CLS_OFFSET, -1);
            GetattrRewriteArgs ga_rewrite_args(rewrite_args->rewriter, r_cls);

            r_cls.assertValid();
            clsattr = getattr_internal(obj->cls, attr->c_str(), false, false, &ga_rewrite_args, NULL);

            if (!ga_rewrite_args.out_success)
                rewrite_args = NULL;
            else if (clsattr)
                r_clsattr = ga_rewrite_args.out_rtn.move(-1);
        } else {
            clsattr = getattr_internal(obj->cls, attr->c_str(), false, false, NULL, NULL);
        }
    }

    if (!clsattr) {
        if (rewrite_args) rewrite_args->out_success = true;
        return NULL;
    }

    if (clsattr->cls == function_cls) {
        if (rewrite_args) {
            r_clsattr.addGuard((int64_t)clsattr);
        }

        // TODO copy from runtimeCall
        // TODO these two branches could probably be folded together (the first one is becoming
        // a subset of the second)
        if (nargs <= 2) {
            Box* rtn;
            if (rewrite_args) {
                CallRewriteArgs srewrite_args(rewrite_args->rewriter, r_clsattr);
                srewrite_args.arg1 = rewrite_args->obj;

                // should be no-ops:
                if (nargs >= 1) srewrite_args.arg2 = rewrite_args->arg1;
                if (nargs >= 2) srewrite_args.arg3 = rewrite_args->arg2;

                srewrite_args.func_guarded = true;
                srewrite_args.args_guarded = true;
                r_clsattr.push();

                rtn = runtimeCallInternal(clsattr, &srewrite_args, nargs+1, obj, arg1, arg2, NULL);

                if (!srewrite_args.out_success) {
                    rewrite_args = NULL;
                } else {
                    r_clsattr = rewrite_args->rewriter->pop(0);
                    rewrite_args->out_rtn = srewrite_args.out_rtn;
                }
            } else {
                rtn = runtimeCallInternal(clsattr, NULL, nargs+1, obj, arg1, arg2, NULL);
            }

            if (rewrite_args) rewrite_args->out_success = true;
            return rtn;
        } else {
            int alloca_size = sizeof(Box*) * (nargs + 1 - 3);

            Box **new_args = (Box**)alloca(alloca_size);
            new_args[0] = arg3;
            memcpy(new_args+1, args, (nargs - 3) * sizeof(Box*));

            Box* rtn;
            if (rewrite_args) {
                const bool annotate = 0;
                if (annotate) rewrite_args->rewriter->trap();
                //if (VERBOSITY()) printf("have to remunge: %d %d %d %d\n", rewrite_args->arg1.getArgnum(), rewrite_args->arg2.getArgnum(), rewrite_args->arg3.getArgnum(), rewrite_args->args.getArgnum());
                // The above line seems to print one of:
                // 4 5 6 7
                // 2 3 4 5
                // Want to move them to
                // 1 2 X X

                //if (nargs >= 1) rewrite_args->arg1 = rewrite_args->arg1.move(1);
                //if (nargs >= 2) rewrite_args->arg2 = rewrite_args->arg2.move(2);
                //if (nargs >= 3) rewrite_args->arg3 = rewrite_args->arg3.move(4);
                //if (nargs >= 4) rewrite_args->args = rewrite_args->args.move(5);

                // There's nothing critical that these are in these registers,
                // just that the register assignments for the rest of this
                // section assume that this is true:
                //assert(rewrite_args->obj.getArgnum() == 0);
                assert(r_clsattr.getArgnum() == -1);

                int new_alloca_reg = -3;
                RewriterVar r_new_args = rewrite_args->rewriter->alloca_(alloca_size, new_alloca_reg);
                r_clsattr.push();

                if (rewrite_args->arg3.isInReg())
                    r_new_args.setAttr(0, rewrite_args->arg3, /* user_visible = */ false);
                else {
                    r_new_args.setAttr(0, rewrite_args->arg3.move(-2), /* user_visible = */ false);
                }

                //arg3 is now dead
                for (int i = 0; i < nargs - 3; i++) {
                    RewriterVar arg;
                    if (rewrite_args->args.isInReg())
                        arg = rewrite_args->args.getAttr(i * sizeof(Box*), -2);
                    else {
                        // TODO this is really bad:
                        arg = rewrite_args->args.move(-2).getAttr(i * sizeof(Box*), -2);
                    }
                    r_new_args.setAttr((i + 1) * sizeof(Box*), arg, /* user_visible = */ false);
                }
                //args is now dead

                CallRewriteArgs srewrite_args(rewrite_args->rewriter, r_clsattr);
                srewrite_args.arg1 = rewrite_args->obj;
                if (nargs >= 1) srewrite_args.arg2 = rewrite_args->arg1;
                if (nargs >= 2) srewrite_args.arg3 = rewrite_args->arg2;
                if (nargs >= 3) srewrite_args.args = r_new_args;
                srewrite_args.args_guarded = true;
                srewrite_args.func_guarded = true;

                if (annotate) rewrite_args->rewriter->annotate(0);
                rtn = runtimeCallInternal(clsattr, &srewrite_args, nargs + 1, obj, arg1, arg2, new_args);
                if (annotate) rewrite_args->rewriter->annotate(1);

                if (!srewrite_args.out_success)
                    rewrite_args = NULL;
                else {
                    r_clsattr = rewrite_args->rewriter->pop(0);
                    rewrite_args->out_rtn = srewrite_args.out_rtn;

                    // TODO should be a dealloca or smth
                    rewrite_args->rewriter->alloca_(-alloca_size, 0);
                    rewrite_args->out_success = true;
                }
                if (annotate) rewrite_args->rewriter->annotate(2);
            } else {
                rtn = runtimeCallInternal(clsattr, NULL, nargs + 1, obj, arg1, arg2, new_args);
            }
            return rtn;
        }
    } else {
        Box* rtn;
        if (clsattr->cls != function_cls)
            rewrite_args = NULL;

        if (rewrite_args) {
            CallRewriteArgs srewrite_args(rewrite_args->rewriter, r_clsattr);
            if (nargs >= 1) srewrite_args.arg1 = rewrite_args->arg1;
            if (nargs >= 2) srewrite_args.arg2 = rewrite_args->arg2;
            if (nargs >= 3) srewrite_args.arg3 = rewrite_args->arg3;
            if (nargs >= 4) srewrite_args.args = rewrite_args->args;
            srewrite_args.args_guarded = true;

            rtn = runtimeCallInternal(clsattr, &srewrite_args, nargs, arg1, arg2, arg3, args);

            if (!srewrite_args.out_success)
                rewrite_args = NULL;
            else
                rewrite_args->out_rtn = srewrite_args.out_rtn;
        } else {
            rtn = runtimeCallInternal(clsattr, NULL, nargs, arg1, arg2, arg3, args);
        }

        if (!rtn) {
            fprintf(stderr, "TypeError: '%s' object is not callable\n", getTypeName(clsattr)->c_str());
            raiseExc();
        }

        if (rewrite_args) rewrite_args->out_success = true;
        return rtn;
    }
}

extern "C" Box* callattr(Box* obj, std::string *attr, bool clsonly, int64_t nargs, Box* arg1, Box* arg2, Box* arg3, Box **args) {
    static StatCounter slowpath_callattr("slowpath_callattr");
    slowpath_callattr.log();

    assert(attr);

    int num_orig_args = 4 + std::min(4L, nargs);
    std::unique_ptr<Rewriter> rewriter(Rewriter::createRewriter(__builtin_extract_return_addr(__builtin_return_address(0)), num_orig_args, 2, "callattr"));
    Box* rtn;

    LookupScope scope = clsonly ? CLASS_ONLY : CLASS_OR_INST;

    if (rewriter.get()) {
        //rewriter->trap();

        // TODO feel weird about doing this; it either isn't necessary
        // or this kind of thing is necessary in a lot more places
        //rewriter->getArg(3).addGuard(nargs);

        CallRewriteArgs rewrite_args(rewriter.get(), rewriter->getArg(0));
        if (nargs >= 1) rewrite_args.arg1 = rewriter->getArg(4);
        if (nargs >= 2) rewrite_args.arg2 = rewriter->getArg(5);
        if (nargs >= 3) rewrite_args.arg3 = rewriter->getArg(6);
        if (nargs >= 4) rewrite_args.args = rewriter->getArg(7);
        rtn = callattrInternal(obj, attr, scope, &rewrite_args, nargs, arg1, arg2, arg3, args);

        if (!rewrite_args.out_success) {
            rewriter.reset(NULL);
        } else if (rtn) {
            rewrite_args.out_rtn.move(-1);
        }
    } else {
        rtn = callattrInternal(obj, attr, scope, NULL, nargs, arg1, arg2, arg3, args);
    }

    if (rtn == NULL) {
        raiseAttributeError(obj, attr->c_str());
    }

    if (rewriter.get()) rewriter->commit();
    return rtn;
}

static const std::string _call_str("__call__"), _new_str("__new__"), _init_str("__init__");
Box* runtimeCallInternal(Box* obj, CallRewriteArgs *rewrite_args, int64_t nargs, Box* arg1, Box* arg2, Box* arg3, Box* *args) {
    // the 10M upper bound isn't a hard max, just almost certainly a bug
    // (also the alloca later will probably fail anyway)
    ASSERT(nargs >= 0 && nargs < 10000000, "%ld", nargs);

    Box* orig_obj = obj;

    if (obj->cls != function_cls && obj->cls != instancemethod_cls) {
        if (rewrite_args) {
            // TODO is this ok?
            //rewrite_args->rewriter->trap();
            return callattrInternal(obj, &_call_str, CLASS_ONLY, rewrite_args, nargs, arg1, arg2, arg3, args);
        } else {
            return callattrInternal(obj, &_call_str, CLASS_ONLY, NULL, nargs, arg1, arg2, arg3, args);
        }
    }

    if (rewrite_args) {
        if (!rewrite_args->args_guarded) {
            // TODO should know which args don't need to be guarded, ex if we're guaranteed that they
            // already fit, either since the type inferencer could determine that,
            // or because they only need to fit into an UNKNOWN slot.

            if (nargs >= 1) rewrite_args->arg1.addAttrGuard(BOX_CLS_OFFSET, (intptr_t)arg1->cls);
            if (nargs >= 2) rewrite_args->arg2.addAttrGuard(BOX_CLS_OFFSET, (intptr_t)arg2->cls);
            if (nargs >= 3) rewrite_args->arg3.addAttrGuard(BOX_CLS_OFFSET, (intptr_t)arg3->cls);
            for (int i = 3; i < nargs; i++) {
                rewrite_args->args.getAttr((i - 3) * sizeof(Box*), -1).addAttrGuard(BOX_CLS_OFFSET, (intptr_t)args[i-3]->cls);
            }
        }

        rewrite_args->rewriter->addDecision(obj->cls == function_cls ? 1 : 0);
    }

    if (obj->cls == function_cls) {
        BoxedFunction *f = static_cast<BoxedFunction*>(obj);

        CompiledFunction *cf = resolveCLFunc(f->f, nargs, arg1, arg2, arg3, args);

        // typeCall (ie the base for constructors) is important enough that it knows
        // how to do rewrites, so lets cut directly to the internal function rather
        // than hitting its python bindings:
        if (cf->code == typeCall) {
            Box* rtn;
            if (rewrite_args) {
                CallRewriteArgs srewrite_args(rewrite_args->rewriter, RewriterVar());
                if (nargs >= 1) srewrite_args.arg1 = rewrite_args->arg1;
                if (nargs >= 2) srewrite_args.arg2 = rewrite_args->arg2;
                if (nargs >= 3) srewrite_args.arg3 = rewrite_args->arg3;
                if (nargs >= 4) srewrite_args.args = rewrite_args->args;
                rtn = typeCallInternal(&srewrite_args, nargs, arg1, arg2, arg3, args);
                if (!srewrite_args.out_success)
                    rewrite_args = NULL;
                else
                    rewrite_args->out_rtn = srewrite_args.out_rtn;
            } else {
                rtn = typeCallInternal(NULL, nargs, arg1, arg2, arg3, args);
            }

            if (rewrite_args) rewrite_args->out_success = true;
            return rtn;
        }

        if (cf->sig->is_vararg) rewrite_args = NULL;
        if (cf->is_interpreted) rewrite_args = NULL;

        if (rewrite_args) {
            if (!rewrite_args->func_guarded)
                rewrite_args->obj.addGuard((intptr_t)obj);

            rewrite_args->rewriter->addDependenceOn(cf->dependent_callsites);

            //if (VERBOSITY()) {
                //printf("runtimeCallInternal: %d", rewrite_args->obj.getArgnum());
                //if (nargs >= 1) printf(" %d", rewrite_args->arg1.getArgnum());
                //if (nargs >= 2) printf(" %d", rewrite_args->arg2.getArgnum());
                //if (nargs >= 3) printf(" %d", rewrite_args->arg3.getArgnum());
                //if (nargs >= 4) printf(" %d", rewrite_args->args.getArgnum());
                //printf("\n");
            //}

            if (nargs >= 1) rewrite_args->arg1.move(0);
            if (nargs >= 2) rewrite_args->arg2.move(1);
            if (nargs >= 3) rewrite_args->arg3.move(2);
            if (nargs >= 4) rewrite_args->args.move(3);
            RewriterVar r_rtn = rewrite_args->rewriter->call(cf->code);
            rewrite_args->out_rtn = r_rtn.move(-1);
        }
        Box* rtn = callCompiledFunc(cf, nargs, arg1, arg2, arg3, args);

        if (rewrite_args) rewrite_args->out_success = true;
        return rtn;
    } else if (obj->cls == instancemethod_cls) {
        // TODO it's dumb but I should implement patchpoints here as well
        // duplicated with callattr
        BoxedInstanceMethod *im = static_cast<BoxedInstanceMethod*>(obj);

        if (rewrite_args && !rewrite_args->func_guarded) {
            rewrite_args->obj.addAttrGuard(INSTANCEMETHOD_FUNC_OFFSET, (intptr_t)im->func);
        }

        if (nargs <= 2) {
            Box* rtn;
            if (rewrite_args) {
                // Kind of weird that we don't need to give this a valid RewriterVar, but it shouldn't need to access it
                // (since we've already guarded on the function).
                CallRewriteArgs srewrite_args(rewrite_args->rewriter, RewriterVar());

                srewrite_args.arg1 = rewrite_args->obj.getAttr(INSTANCEMETHOD_OBJ_OFFSET, 0);
                srewrite_args.func_guarded = true;
                srewrite_args.args_guarded = true;
                if (nargs >= 1) srewrite_args.arg2 = rewrite_args->arg1;
                if (nargs >= 2) srewrite_args.arg3 = rewrite_args->arg2;

                rtn = runtimeCallInternal(im->func, &srewrite_args, nargs+1, im->obj, arg1, arg2, NULL);

                if (!srewrite_args.out_success) {
                    rewrite_args = NULL;
                } else {
                    rewrite_args->out_rtn = srewrite_args.out_rtn.move(-1);
                }
            } else {
                rtn = runtimeCallInternal(im->func, NULL, nargs+1, im->obj, arg1, arg2, NULL);
            }
            if (rewrite_args) rewrite_args->out_success = true;
            return rtn;
        } else {
            Box **new_args = (Box**)alloca(sizeof(Box*) * (nargs + 1 - 3));
            new_args[0] = arg3;
            memcpy(new_args+1, args, (nargs - 3) * sizeof(Box*));
            Box* rtn = runtimeCall(im->func, nargs + 1, im->obj, arg1, arg2, new_args);
            return rtn;
        }
    }
    assert(0);
    abort();
}

extern "C" Box* runtimeCall(Box *obj, int64_t nargs, Box* arg1, Box* arg2, Box* arg3, Box **args) {
    static StatCounter slowpath_runtimecall("slowpath_runtimecall");
    slowpath_runtimecall.log();

    int num_orig_args = 2 + std::min(4L, nargs);
    std::unique_ptr<Rewriter> rewriter(Rewriter::createRewriter(__builtin_extract_return_addr(__builtin_return_address(0)), num_orig_args, 2, "runtimeCall"));
    Box* rtn;

    if (rewriter.get()) {
        //rewriter->trap();

        // TODO feel weird about doing this; it either isn't necessary
        // or this kind of thing is necessary in a lot more places
        //rewriter->getArg(1).addGuard(nargs);

        CallRewriteArgs rewrite_args(rewriter.get(), rewriter->getArg(0));
        if (nargs >= 1) rewrite_args.arg1 = rewriter->getArg(2);
        if (nargs >= 2) rewrite_args.arg2 = rewriter->getArg(3);
        if (nargs >= 3) rewrite_args.arg3 = rewriter->getArg(4);
        if (nargs >= 4) rewrite_args.args = rewriter->getArg(5);
        rtn = runtimeCallInternal(obj, &rewrite_args, nargs, arg1, arg2, arg3, args);

        if (!rewrite_args.out_success) {
            rewriter.reset(NULL);
        } else if (rtn) {
            rewrite_args.out_rtn.move(-1);
        }
    } else {
        rtn = runtimeCallInternal(obj, NULL, nargs, arg1, arg2, arg3, args);
    }

    if (rewriter.get()) {
        rewriter->commit();
    }
    return rtn;
}

extern "C" Box* binopInternal(Box* lhs, Box* rhs, int op_type, bool inplace, BinopRewriteArgs *rewrite_args) {
    // TODO handle the case of the rhs being a subclass of the lhs
    // this could get really annoying because you can dynamically make one type a subclass
    // of the other!

    if (rewrite_args) {
        //rewriter->trap();

        RewriterVar r_lhs = rewrite_args->rewriter->getArg(0);
        RewriterVar r_rhs = rewrite_args->rewriter->getArg(1);
        // TODO probably don't need to guard on the lhs_cls since it
        // will get checked no matter what, but the check that should be
        // removed is probably the later one.
        // ie we should have some way of specifying what we know about the values
        // of objects and their attributes, and the attributes' attributes.
        r_lhs.addAttrGuard(BOX_CLS_OFFSET, (intptr_t)lhs->cls);
        r_rhs.addAttrGuard(BOX_CLS_OFFSET, (intptr_t)rhs->cls);
    }

    std::string iop_name = getInplaceOpName(op_type);
    Box* irtn = NULL;
    if (inplace) {
        if (rewrite_args) {
            CallRewriteArgs srewrite_args(rewrite_args->rewriter, rewrite_args->lhs);
            srewrite_args.arg1 = rewrite_args->rhs;
            irtn = callattrInternal1(lhs, &iop_name, CLASS_ONLY, &srewrite_args, 1, rhs);

            if (!srewrite_args.out_success)
                rewrite_args = NULL;
            else if (irtn)
                rewrite_args->out_rtn = srewrite_args.out_rtn.move(-1);
        } else {
            irtn = callattrInternal1(lhs, &iop_name, CLASS_ONLY, NULL, 1, rhs);
        }

        if (irtn) {
            if (irtn != NotImplemented) {
                if (rewrite_args) {
                    rewrite_args->out_success = true;
                }
                return irtn;
            }
        }
    }



    std::string op_name = getOpName(op_type);
    Box* lrtn;
    if (rewrite_args) {
        CallRewriteArgs srewrite_args(rewrite_args->rewriter, rewrite_args->lhs);
        srewrite_args.arg1 = rewrite_args->rhs;
        lrtn = callattrInternal1(lhs, &op_name, CLASS_ONLY, &srewrite_args, 1, rhs);

        if (!srewrite_args.out_success)
            rewrite_args = NULL;
        else if (lrtn)
            rewrite_args->out_rtn = srewrite_args.out_rtn.move(-1);
    } else {
        lrtn = callattrInternal1(lhs, &op_name, CLASS_ONLY, NULL, 1, rhs);
    }


    if (lrtn) {
        if (lrtn != NotImplemented) {
            if (rewrite_args) {
                rewrite_args->out_success = true;
            }
            return lrtn;
        }
    }

    // TODO patch these cases

    std::string rop_name = getReverseOpName(op_type);
    Box* rattr_func = getattr_internal(rhs->cls, rop_name.c_str(), false, false, NULL, NULL);
    if (rattr_func) {
        Box* rtn = runtimeCall2(rattr_func, 2, rhs, lhs);
        if (rtn != NotImplemented) {
            return rtn;
        }
    } else {
        //printf("rfunc doesn't exist\n");
    }

    if (inplace) {
        fprintf(stderr, "TypeError: unsupported operand type(s) for %s: '%s' and '%s'\n", getInplaceOpSymbol(op_type).c_str(), getTypeName(lhs)->c_str(), getTypeName(rhs)->c_str());
    } else {
        fprintf(stderr, "TypeError: unsupported operand type(s) for %s: '%s' and '%s'\n", getOpSymbol(op_type).c_str(), getTypeName(lhs)->c_str(), getTypeName(rhs)->c_str());
    }
    if (VERBOSITY()) {
        if (inplace) {
            if (irtn)
                fprintf(stderr, "%s has %s, but returned NotImplemented\n", getTypeName(lhs)->c_str(), iop_name.c_str());
            else
                fprintf(stderr, "%s does not have %s\n", getTypeName(lhs)->c_str(), iop_name.c_str());
        }

        if (lrtn)
            fprintf(stderr, "%s has %s, but returned NotImplemented\n", getTypeName(lhs)->c_str(), op_name.c_str());
        else
            fprintf(stderr, "%s does not have %s\n", getTypeName(lhs)->c_str(), op_name.c_str());
        if (rattr_func)
            fprintf(stderr, "%s has %s, but returned NotImplemented\n", getTypeName(rhs)->c_str(), rop_name.c_str());
        else
            fprintf(stderr, "%s does not have %s\n", getTypeName(rhs)->c_str(), rop_name.c_str());
    }
    raiseExc();
}

extern "C" Box* binop(Box* lhs, Box* rhs, int op_type) {
    static StatCounter slowpath_binop("slowpath_binop");
    slowpath_binop.log();
    //static StatCounter nopatch_binop("nopatch_binop");

    //int id = Stats::getStatId("slowpath_binop_" + *getTypeName(lhs) + op_name + *getTypeName(rhs));
    //Stats::log(id);

    std::unique_ptr<Rewriter> rewriter((Rewriter*)NULL);
    // Currently can't patchpoint user-defined binops since we can't assume that just because
    // resolving it one way right now (ex, using the value from lhs.__add__) means that later
    // we'll resolve it the same way, even for the same argument types.
    // TODO implement full resolving semantics inside the rewrite?
    bool can_patchpoint = !isUserDefined(lhs->cls) && !isUserDefined(rhs->cls);
    if (can_patchpoint)
        rewriter.reset(Rewriter::createRewriter(__builtin_extract_return_addr(__builtin_return_address(0)), 3, 1, "binop"));

    Box* rtn;
    if (rewriter.get()) {
        //rewriter->trap();
        BinopRewriteArgs rewrite_args(rewriter.get(), rewriter->getArg(0), rewriter->getArg(1));
        rtn = binopInternal(lhs, rhs, op_type, false, &rewrite_args);
        assert(rtn);
        if (!rewrite_args.out_success)
            rewriter.reset(NULL);
        else
            rewrite_args.out_rtn.move(-1);
    } else {
        rtn = binopInternal(lhs, rhs, op_type, false, NULL);
    }

    if (rewriter.get()) {
        rewriter->commit();
    }

    return rtn;
}

extern "C" Box* augbinop(Box* lhs, Box* rhs, int op_type) {
    static StatCounter slowpath_binop("slowpath_binop");
    slowpath_binop.log();
    //static StatCounter nopatch_binop("nopatch_binop");

    //int id = Stats::getStatId("slowpath_binop_" + *getTypeName(lhs) + op_name + *getTypeName(rhs));
    //Stats::log(id);

    std::unique_ptr<Rewriter> rewriter((Rewriter*)NULL);
    // Currently can't patchpoint user-defined binops since we can't assume that just because
    // resolving it one way right now (ex, using the value from lhs.__add__) means that later
    // we'll resolve it the same way, even for the same argument types.
    // TODO implement full resolving semantics inside the rewrite?
    bool can_patchpoint = !isUserDefined(lhs->cls) && !isUserDefined(rhs->cls);
    if (can_patchpoint)
        rewriter.reset(Rewriter::createRewriter(__builtin_extract_return_addr(__builtin_return_address(0)), 3, 1, "binop"));

    Box* rtn;
    if (rewriter.get()) {
        //rewriter->trap();
        BinopRewriteArgs rewrite_args(rewriter.get(), rewriter->getArg(0), rewriter->getArg(1));
        rtn = binopInternal(lhs, rhs, op_type, true, &rewrite_args);
        if (!rewrite_args.out_success)
            rewriter.reset(NULL);
        else
            rewrite_args.out_rtn.move(-1);
    } else {
        rtn = binopInternal(lhs, rhs, op_type, true, NULL);
    }

    if (rewriter.get()) {
        rewriter->commit();
    }

    return rtn;
}

Box* compareInternal(Box* lhs, Box* rhs, int op_type, CompareRewriteArgs *rewrite_args) {
    if (op_type == AST_TYPE::Is || op_type == AST_TYPE::IsNot) {
        bool neg = (op_type == AST_TYPE::IsNot);

        if (rewrite_args) {
            if (neg)
                rewrite_args->lhs.cmp(AST_TYPE::NotEq, rewrite_args->rhs, 0);
            else
                rewrite_args->lhs.cmp(AST_TYPE::Eq, rewrite_args->rhs, 0);
            rewrite_args->out_rtn = rewrite_args->rewriter->call((void*)boxBool);
            rewrite_args->out_success = true;
        }

        return boxBool((lhs == rhs) ^ neg);
    }

    // Can do the guard checks after the Is/IsNot handling, since that is
    // irrespective of the object classes
    if (rewrite_args) {
        // TODO probably don't need to guard on the lhs_cls since it
        // will get checked no matter what, but the check that should be
        // removed is probably the later one.
        // ie we should have some way of specifying what we know about the values
        // of objects and their attributes, and the attributes' attributes.
        rewrite_args->lhs.addAttrGuard(BOX_CLS_OFFSET, (intptr_t)lhs->cls);
        rewrite_args->rhs.addAttrGuard(BOX_CLS_OFFSET, (intptr_t)rhs->cls);
    }

    std::string op_name = getOpName(op_type);

    Box* lrtn;
    if (rewrite_args) {
        CallRewriteArgs crewrite_args(rewrite_args->rewriter, rewrite_args->lhs);
        crewrite_args.arg1 = rewrite_args->rhs;
        lrtn = callattrInternal1(lhs, &op_name, CLASS_ONLY, &crewrite_args, 1, rhs);

        if (!crewrite_args.out_success)
            rewrite_args = NULL;
        else if (lrtn)
            rewrite_args->out_rtn = crewrite_args.out_rtn;
    } else {
        lrtn = callattrInternal1(lhs, &op_name, CLASS_ONLY, NULL, 1, rhs);
    }

    if (lrtn) {
        if (lrtn != NotImplemented) {
            bool can_patchpoint = !isUserDefined(lhs->cls) && !isUserDefined(rhs->cls);
            if (rewrite_args && can_patchpoint) {
                rewrite_args->out_success = true;
            }
            return lrtn;
        }
    } else {
    }

    std::string rop_name = getReverseOpName(op_type);
    Box* rattr_func = getattr_internal(rhs->cls, rop_name.c_str(), false, false, NULL, NULL);
    if (rattr_func) {
        Box* rtn = runtimeCall2(rattr_func, 2, rhs, lhs);
        if (rtn != NotImplemented) {
            ////printf("rfunc returned NotImplemented\n");
            ////bool can_patchpoint = lhs->cls->is_constant && (lattr_func == NULL || (lattr;
            //bool can_patchpoint = !isUserDefined(lhs->cls) && lhs->cls->is_constant;
            //if (can_patchpoint && rhs->cls->is_constant && rattr_func->cls == function_cls) {
                //void* rtn_addr = __builtin_extract_return_addr(__builtin_return_address(0));
                //_repatchBinExp(rtn_addr, true, lhs, rhs, rattr_func);
            //}
            return rtn;
        }
    } else {
        //printf("rfunc doesn't exist\n");
    }


    if (op_type == AST_TYPE::Eq)
        return boxBool(lhs == rhs);
    if (op_type == AST_TYPE::NotEq)
        return boxBool(lhs != rhs);

    // TODO
    // According to http://docs.python.org/2/library/stdtypes.html#comparisons
    // CPython implementation detail: Objects of different types except numbers are ordered by their type names; objects of the same types that don’t support proper comparison are ordered by their address.

    if (op_type == AST_TYPE::Gt || op_type == AST_TYPE::GtE || op_type == AST_TYPE::Lt || op_type == AST_TYPE::LtE) {
        intptr_t cmp1, cmp2;
        if (lhs->cls == rhs->cls) {
            cmp1 = (intptr_t)lhs;
            cmp2 = (intptr_t)rhs;
        } else {
            // This isn't really necessary, but try to make sure that numbers get sorted first
            if (lhs->cls == int_cls || lhs->cls == float_cls)
                cmp1 = 0;
            else
                cmp1 = (intptr_t)lhs->cls;
            if (rhs->cls == int_cls || rhs->cls == float_cls)
                cmp2 = 0;
            else
                cmp2 = (intptr_t)rhs->cls;
        }

        if (op_type == AST_TYPE::Gt)
            return boxBool(cmp1 > cmp2);
        if (op_type == AST_TYPE::GtE)
            return boxBool(cmp1 >= cmp2);
        if (op_type == AST_TYPE::Lt)
            return boxBool(cmp1 < cmp2);
        if (op_type == AST_TYPE::LtE)
            return boxBool(cmp1 <= cmp2);
    }
    RELEASE_ASSERT(0, "");
}

extern "C" Box* compare(Box* lhs, Box* rhs, int op_type) {
    static StatCounter slowpath_compare("slowpath_compare");
    slowpath_compare.log();
    static StatCounter nopatch_compare("nopatch_compare");

    std::unique_ptr<Rewriter> rewriter(Rewriter::createRewriter(__builtin_extract_return_addr(__builtin_return_address(0)), 3, 1, "compare"));

    Box* rtn;
    if (rewriter.get()) {
        //rewriter->trap();
        CompareRewriteArgs rewrite_args(rewriter.get(), rewriter->getArg(0), rewriter->getArg(1));
        rtn = compareInternal(lhs, rhs, op_type, &rewrite_args);
        if (!rewrite_args.out_success)
            rewriter.reset(NULL);
        else
            rewrite_args.out_rtn.move(-1);
    } else {
        rtn = compareInternal(lhs, rhs, op_type, NULL);
    }

    if (rewriter.get()) {
        rewriter->commit();
    }

    return rtn;
}

extern "C" Box* unaryop(Box* operand, int op_type) {
    static StatCounter slowpath_unaryop("slowpath_unaryop");
    slowpath_unaryop.log();

    std::string op_name = getOpName(op_type);

    Box* attr_func = getclsattr_internal(operand, op_name.c_str(), NULL, NULL);

    ASSERT(attr_func, "%s.%s", getTypeName(operand)->c_str(), op_name.c_str());

    Box* rtn = runtimeCall0(attr_func, 0);
    return rtn;
}

extern "C" Box* getitem(Box* value, Box* slice) {
    static StatCounter slowpath_getitem("slowpath_getitem");
    slowpath_getitem.log();
    static std::string str_getitem("__getitem__");

    std::unique_ptr<Rewriter> rewriter(Rewriter::createRewriter(__builtin_extract_return_addr(__builtin_return_address(0)), 2, 1, "getitem"));

    Box* rtn;
    if (rewriter.get()) {
        CallRewriteArgs rewrite_args(rewriter.get(), rewriter->getArg(0));
        rewrite_args.arg1 = rewriter->getArg(1);

        rtn = callattrInternal1(value, &str_getitem, CLASS_ONLY, &rewrite_args, 1, slice);

        if (!rewrite_args.out_success)
            rewriter.reset(NULL);
        else if (rtn)
            rewrite_args.out_rtn.move(-1);
    } else {
        rtn = callattrInternal1(value, &str_getitem, CLASS_ONLY, NULL, 1, slice);
    }

    if (rtn == NULL) {
        // different versions of python give different error messages for this:
        if (PYTHON_VERSION_MAJOR == 2 && PYTHON_VERSION_MINOR < 7) {
            fprintf(stderr, "TypeError: '%s' object is unsubscriptable\n", getTypeName(value)->c_str()); // 2.6.6
        } else if (PYTHON_VERSION_MAJOR == 2 && PYTHON_VERSION_MINOR == 7 && PYTHON_VERSION_MICRO < 3) {
            fprintf(stderr, "TypeError: '%s' object is not subscriptable\n", getTypeName(value)->c_str()); // 2.7.1
        } else {
            fprintf(stderr, "TypeError: '%s' object has no attribute '__getitem__'\n", getTypeName(value)->c_str()); // 2.7.3
        }
        raiseExc();
    }

    if (rewriter.get()) rewriter->commit();
    return rtn;
}

// target[slice] = value
extern "C" void setitem(Box* target, Box* slice, Box* value) {
    static StatCounter slowpath_setitem("slowpath_setitem");
    slowpath_setitem.log();
    static std::string str_setitem("__setitem__");

    std::unique_ptr<Rewriter> rewriter(Rewriter::createRewriter(__builtin_extract_return_addr(__builtin_return_address(0)), 3, 1, "setitem"));

    Box* rtn;
    RewriterVar r_rtn;
    if (rewriter.get()) {
        CallRewriteArgs rewrite_args(rewriter.get(), rewriter->getArg(0));
        rewrite_args.arg1 = rewriter->getArg(1);
        rewrite_args.arg2 = rewriter->getArg(2);

        rtn = callattrInternal2(target, &str_setitem, CLASS_ONLY, &rewrite_args, 2, slice, value);

        if (!rewrite_args.out_success)
            rewriter.reset(NULL);
        else if (rtn)
            r_rtn = rewrite_args.out_rtn;
    } else {
        rtn = callattrInternal2(target, &str_setitem, CLASS_ONLY, NULL, 2, slice, value);
    }

    if (rtn == NULL) {
        fprintf(stderr, "TypeError: '%s' object does not support item assignment\n", getTypeName(target)->c_str());
        raiseExc();
    }

    if (rewriter.get()) {
        rewriter->commit();
    }
}

// del target[slice]
extern "C" void delitem(Box* target, Box* slice) {
    static StatCounter slowpath_delitem("slowpath_delitem");
    slowpath_delitem.log();
    static std::string str_setitem("__delitem__");

    //not sure about the temporal register number
    std::unique_ptr<Rewriter> rewriter(Rewriter::createRewriter(__builtin_extract_return_addr(__builtin_return_address(0)), 2, 1, "delitem"));

    Box* rtn;
    RewriterVar r_rtn;
    if (rewriter.get()) {
      //?true
        CallRewriteArgs rewrite_args(rewriter.get(), rewriter->getArg(0));
        rewrite_args.arg1 = rewriter->getArg(1);
       
        rtn = callattrInternal1(target, &str_setitem, CLASS_ONLY, &rewrite_args, 1, slice);

        if (!rewrite_args.out_success)
            rewriter.reset(NULL);
        else if (rtn)
            r_rtn = rewrite_args.out_rtn;
    } else {
        rtn = callattrInternal1(target, &str_setitem, CLASS_ONLY, NULL, 1, slice);
    }

    if (rtn == NULL) {
      //TODO provide the correct error here
        fprintf(stderr, "TODO TypeError: '%s' doesn't support del\n", getTypeName(target)->c_str());
        raiseExc();
    }

    if (rewriter.get()) {
        rewriter->commit();
    }
}

// A wrapper around the HCBox constructor
// TODO is there a way to avoid the indirection?
static Box* makeHCBox(ObjectFlavor *flavor, BoxedClass *cls) {
    return new HCBox(flavor, cls);
}

// For use on __init__ return values
static void assertInitNone(Box *obj) {
    if (obj != None) {
        fprintf(stderr, "TypeError: __init__() should return None, not '%s'\n", getTypeName(obj)->c_str());
        raiseExc();
    }
}

Box* typeCallInternal(CallRewriteArgs *rewrite_args, int64_t nargs, Box* arg1, Box* arg2, Box* arg3, Box** args) {
    static StatCounter slowpath_typecall("slowpath_typecall");
    slowpath_typecall.log();

    //if (rewrite_args && VERBOSITY()) {
        //printf("typeCallInternal: %d", rewrite_args->obj.getArgnum());
        //if (nargs >= 1) printf(" %d", rewrite_args->arg1.getArgnum());
        //if (nargs >= 2) printf(" %d", rewrite_args->arg2.getArgnum());
        //if (nargs >= 3) printf(" %d", rewrite_args->arg3.getArgnum());
        //if (nargs >= 4) printf(" %d", rewrite_args->args.getArgnum());
        //printf("\n");
    //}


    RewriterVar r_ccls, r_new, r_init;
    Box *new_attr, *init_attr;
    if (rewrite_args) {
        //rewrite_args->rewriter->annotate(0);
        //rewrite_args->rewriter->trap();
        r_ccls = rewrite_args->arg1;
        // This is probably a duplicate, but it's hard to really convince myself of that.
        // Need to create a clear contract of who guards on what
        r_ccls.addGuard((intptr_t)arg1);
    }

    Box* cls = arg1;
    if (cls->cls != type_cls) {
        fprintf(stderr, "TypeError: descriptor '__call__' requires a 'type' object but received an '%s'\n", getTypeName(cls)->c_str());
        raiseExc();
    }

    BoxedClass* ccls = static_cast<BoxedClass*>(cls);

    if (rewrite_args) {
        GetattrRewriteArgs grewrite_args(rewrite_args->rewriter, r_ccls);
        grewrite_args.preferred_dest_reg = -2;
        new_attr = getattr_internal(ccls, "__new__", false, false, &grewrite_args, NULL);

        if (!grewrite_args.out_success)
            rewrite_args = NULL;
        else {
            if (new_attr) {
                r_new = grewrite_args.out_rtn.move(-2);
                r_new.addGuard((intptr_t)new_attr);
            }
        }
    } else {
        new_attr = getattr_internal(ccls, "__new__", false, false, NULL, NULL);
    }

    if (rewrite_args) {
        GetattrRewriteArgs grewrite_args(rewrite_args->rewriter, r_ccls);
        init_attr = getattr_internal(ccls, "__init__", false, false, &grewrite_args, NULL);

        if (!grewrite_args.out_success)
            rewrite_args = NULL;
        else {
            if (init_attr) {
                r_init = grewrite_args.out_rtn;
                r_init.addGuard((intptr_t)init_attr);
            }
        }
    } else {
        init_attr = getattr_internal(ccls, "__init__", false, false, NULL, NULL);
    }

    //Box* made = callattrInternal(ccls, &_new_str, INST_ONLY, NULL, nargs, cls, arg2, arg3, args);
    Box* made;
    RewriterVar r_made;
    if (new_attr) {
        if (rewrite_args) {
            if (init_attr) r_init.push();
            if (nargs >= 1) r_ccls.push();
            if (nargs >= 2) rewrite_args->arg2.push();
            if (nargs >= 3) rewrite_args->arg3.push();
            if (nargs >= 4) rewrite_args->args.push();

            CallRewriteArgs srewrite_args(rewrite_args->rewriter, r_new);
            if (nargs >= 1) srewrite_args.arg1 = r_ccls;
            if (nargs >= 2) srewrite_args.arg2 = rewrite_args->arg2;
            if (nargs >= 3) srewrite_args.arg3 = rewrite_args->arg3;
            if (nargs >= 4) srewrite_args.args = rewrite_args->args;
            srewrite_args.args_guarded = true;
            srewrite_args.func_guarded = true;

            r_new.push();

            made = runtimeCallInternal(new_attr, &srewrite_args, nargs, cls, arg2, arg3, args);

            if (!srewrite_args.out_success)
                rewrite_args = NULL;
            else {
                r_made = srewrite_args.out_rtn;

                r_new = rewrite_args->rewriter->pop(0);
                r_made = r_made.move(-1);

                if (nargs >= 4) rewrite_args->args = rewrite_args->rewriter->pop(3);
                if (nargs >= 3) rewrite_args->arg3 = rewrite_args->rewriter->pop(2);
                if (nargs >= 2) rewrite_args->arg2 = rewrite_args->rewriter->pop(1);
                if (nargs >= 1) r_ccls = rewrite_args->arg1 = rewrite_args->rewriter->pop(0);
                if (init_attr) r_init = rewrite_args->rewriter->pop(-2);
            }
        } else {
            made = runtimeCallInternal(new_attr, NULL, nargs, cls, arg2, arg3, args);
        }
    } else {
        if (isUserDefined(ccls)) {
            made = new HCBox(&user_flavor, ccls);

            if (rewrite_args) {
                if (init_attr) r_init.push();
                if (nargs >= 1) r_ccls.push();
                if (nargs >= 2) rewrite_args->arg2.push();
                if (nargs >= 3) rewrite_args->arg3.push();
                if (nargs >= 4) rewrite_args->args.push();

                r_ccls.move(1);
                rewrite_args->rewriter->loadConst(0, (intptr_t)&user_flavor);
                r_made = rewrite_args->rewriter->call((void*)&makeHCBox);

                if (nargs >= 4) rewrite_args->args = rewrite_args->rewriter->pop(3);
                if (nargs >= 3) rewrite_args->arg3 = rewrite_args->rewriter->pop(2);
                if (nargs >= 2) rewrite_args->arg2 = rewrite_args->rewriter->pop(1);
                if (nargs >= 1) r_ccls = rewrite_args->arg1 = rewrite_args->rewriter->pop(0);
                if (init_attr) r_init = rewrite_args->rewriter->pop(-2);
            }
        } else {
            // Not sure what type of object to make here; maybe an HCBox? would be disastrous if it ever
            // made the wrong one though, so just err for now:
            fprintf(stderr, "no __new__ defined for %s!\n", getNameOfClass(ccls)->c_str());
            raiseExc();
        }
    }

    assert(made);
    // If this is true, not supposed to call __init__:
    assert(made->cls == ccls && "allowed but unsupported");

    if (init_attr) {
        Box* initrtn;
        if (rewrite_args) {
            CallRewriteArgs srewrite_args(rewrite_args->rewriter, r_init);
            if (nargs >= 1) srewrite_args.arg1 = r_made;
            if (nargs >= 2) srewrite_args.arg2 = rewrite_args->arg2;
            if (nargs >= 3) srewrite_args.arg3 = rewrite_args->arg3;
            if (nargs >= 4) srewrite_args.args = rewrite_args->args;
            srewrite_args.args_guarded = true;
            srewrite_args.func_guarded = true;

            r_made.push();
            r_init.push();
            //initrtn = callattrInternal(ccls, &_init_str, INST_ONLY, &srewrite_args, nargs, made, arg2, arg3, args);
            initrtn = runtimeCallInternal(init_attr, &srewrite_args, nargs, made, arg2, arg3, args);

            if (!srewrite_args.out_success)
                rewrite_args = NULL;
            else {
                srewrite_args.out_rtn.move(0);
                rewrite_args->rewriter->call((void*)assertInitNone);

                r_init = rewrite_args->rewriter->pop(0);
                r_made = rewrite_args->rewriter->pop(-1);
            }
        } else {
            //initrtn = callattrInternal(ccls, &_init_str, INST_ONLY, NULL, nargs, made, arg2, arg3, args);
            initrtn = runtimeCallInternal(init_attr, NULL, nargs, made, arg2, arg3, args);
        }
        assertInitNone(initrtn);
    } else {
        if (new_attr == NULL && nargs != 1) {
            fprintf(stderr, "TypeError: object.__new__() takes no parameters\n");
            raiseExc();
        }
    }

    if (rewrite_args) {
        rewrite_args->out_rtn = r_made;
        rewrite_args->out_success = true;
    }
    return made;
}

Box* typeCall(Box* obj, BoxedList* vararg) {
    assert(vararg->cls == list_cls);
    if (vararg->size == 0)
        return typeCallInternal1(NULL, 1, obj);
    else if (vararg->size == 1)
        return typeCallInternal2(NULL, 2, obj, vararg->elts->elts[0]);
    else if (vararg->size == 2)
        return typeCallInternal3(NULL, 3, obj, vararg->elts->elts[0], vararg->elts->elts[1]);
    else
        return typeCallInternal(NULL, 1 + vararg->size, obj, vararg->elts->elts[0], vararg->elts->elts[1], &vararg->elts->elts[2]);
}

Box* typeNew(Box* cls, Box* obj) {
    assert(cls == type_cls);

    BoxedClass *rtn = obj->cls;
    return rtn;
}

extern "C" Box* getGlobal(BoxedModule* m, std::string *name, bool from_global) {
    static StatCounter slowpath_getglobal("slowpath_getglobal");
    slowpath_getglobal.log();
    static StatCounter nopatch_getglobal("nopatch_getglobal");

    if (VERBOSITY() >= 2) {
        std::string per_name_stat_name = "getglobal__" + *name;
        int id = Stats::getStatId(per_name_stat_name);
        Stats::log(id);
    }

    { /* anonymous scope to make sure destructors get run before we err out */
        std::unique_ptr<Rewriter> rewriter(Rewriter::createRewriter(__builtin_extract_return_addr(__builtin_return_address(0)), 3, 1, "getGlobal"));

        Box *r;
        if (rewriter.get()) {
            //rewriter->trap();

            GetattrRewriteArgs rewrite_args(rewriter.get(), rewriter->getArg(0));
            r = m->getattr(*name, &rewrite_args, NULL);
            if (!rewrite_args.out_success)
                rewriter.reset(NULL);
        } else {
            r = m->getattr(*name, NULL, NULL);
            nopatch_getglobal.log();
        }

        if (r) {
            if (rewriter.get()) {
                rewriter->commit();
            }
            return r;
        }


        static StatCounter stat_builtins("getglobal_builtins");
        stat_builtins.log();

        if ((*name) == "__builtins__") {
            if (rewriter.get()) {
                RewriterVar r_rtn = rewriter->loadConst(-1, (intptr_t)builtins_module);
                rewriter->commit();
            }
            return builtins_module;
        }

        Box* rtn;
        if (rewriter.get()) {
            RewriterVar builtins = rewriter->loadConst(3, (intptr_t)builtins_module);
            GetattrRewriteArgs rewrite_args(rewriter.get(), builtins);
            rtn = builtins_module->getattr(*name, &rewrite_args, NULL);

            if (!rewrite_args.out_success)
                rewriter.reset(NULL);
        } else {
            rtn = builtins_module->getattr(*name, NULL, NULL);
        }

        if (rewriter.get()) {
            rewriter->commit();
        }

        if (rtn)
            return rtn;
    }

    if (from_global)
        fprintf(stderr, "NameError: name '%s' is not defined\n", name->c_str());
    else
        fprintf(stderr, "NameError: global name '%s' is not defined\n", name->c_str());
    raiseExc();
}

// TODO I feel like importing should go somewhere else; it's more closely tied to codegen
// than to the object model.
extern "C" Box* import(const std::string *name) {
    assert(name);

    static StatCounter slowpath_import("slowpath_import");
    slowpath_import.log();

    BoxedDict *sys_modules = getSysModulesDict();
    Box *s = boxStringPtr(name);
    if (sys_modules->d.find(s) != sys_modules->d.end())
        return sys_modules->d[s];

    BoxedList *sys_path = getSysPath();
    if (sys_path->cls != list_cls) {
        fprintf(stderr, "RuntimeError: sys.path must be a list of directory name\n");
        raiseExc();
    }

    llvm::SmallString<128> joined_path;
    for (int i = 0; i < sys_path->size; i++) {
        Box* _p = sys_path->elts->elts[i];
        if (_p->cls != str_cls)
            continue;
        BoxedString *p = static_cast<BoxedString*>(_p);

        joined_path.clear();
        llvm::sys::path::append(joined_path, p->s, *name + ".py");
        std::string fn(joined_path.str());

        if (VERBOSITY() >= 2) printf("Searching for %s at %s...\n", name->c_str(), fn.c_str());

        bool exists;
        llvm::error_code code = llvm::sys::fs::exists(joined_path.str(), exists);
        assert(code == 0);
        if (!exists)
            continue;

        if (VERBOSITY() >= 1) printf("Beginning import of %s...\n", fn.c_str());

        // TODO duplication with jit.cpp:
        BoxedModule* module = createModule(*name, fn);
        AST_Module* ast = caching_parse(fn.c_str());
        compileAndRunModule(ast, module);
        return module;
    }

    if (*name == "test") {
        return getTestModule();
    }

    fprintf(stderr, "ImportError: No module named %s\n", name->c_str());
    raiseExc();
}


}
