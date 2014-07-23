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

#include "runtime/objmodel.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <stdint.h>

#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"

#include "asm_writing/icinfo.h"
#include "asm_writing/rewriter.h"
#include "asm_writing/rewriter2.h"
#include "codegen/compvars.h"
#include "codegen/irgen/hooks.h"
#include "codegen/llvm_interpreter.h"
#include "codegen/parser.h"
#include "codegen/type_recording.h"
#include "core/ast.h"
#include "core/options.h"
#include "core/stats.h"
#include "core/types.h"
#include "gc/collector.h"
#include "gc/heap.h"
#include "runtime/capi.h"
#include "runtime/float.h"
#include "runtime/gc_runtime.h"
#include "runtime/types.h"
#include "runtime/util.h"

#define BOX_CLS_OFFSET ((char*)&(((Box*)0x01)->cls) - (char*)0x1)
#define HCATTRS_HCLS_OFFSET ((char*)&(((HCAttrs*)0x01)->hcls) - (char*)0x1)
#define HCATTRS_ATTRS_OFFSET ((char*)&(((HCAttrs*)0x01)->attr_list) - (char*)0x1)
#define ATTRLIST_ATTRS_OFFSET ((char*)&(((HCAttrs::AttrList*)0x01)->attrs) - (char*)0x1)
#define ATTRLIST_KIND_OFFSET ((char*)&(((HCAttrs::AttrList*)0x01)->gc_header.kind_id) - (char*)0x1)
#define INSTANCEMETHOD_FUNC_OFFSET ((char*)&(((BoxedInstanceMethod*)0x01)->func) - (char*)0x1)
#define INSTANCEMETHOD_OBJ_OFFSET ((char*)&(((BoxedInstanceMethod*)0x01)->obj) - (char*)0x1)
#define BOOL_B_OFFSET ((char*)&(((BoxedBool*)0x01)->b) - (char*)0x1)
#define INT_N_OFFSET ((char*)&(((BoxedInt*)0x01)->n) - (char*)0x1)

namespace pyston {

struct GetattrRewriteArgs {
    Rewriter* rewriter;
    RewriterVar obj;
    bool out_success;
    RewriterVar out_rtn;

    bool obj_hcls_guarded;
    int preferred_dest_reg;

    GetattrRewriteArgs(Rewriter* rewriter, const RewriterVar& obj)
        : rewriter(rewriter), obj(obj), out_success(false), obj_hcls_guarded(false), preferred_dest_reg(-1) {}
};

struct GetattrRewriteArgs2 {
    Rewriter2* rewriter;
    RewriterVarUsage2 obj;
    Location destination;
    bool more_guards_after;

    bool out_success;
    RewriterVarUsage2 out_rtn;

    bool obj_hcls_guarded;

    GetattrRewriteArgs2(Rewriter2* rewriter, RewriterVarUsage2&& obj, Location destination, bool more_guards_after)
        : rewriter(rewriter), obj(std::move(obj)), destination(destination), more_guards_after(more_guards_after),
          out_success(false), out_rtn(RewriterVarUsage2::empty()), obj_hcls_guarded(false) {}
};

struct SetattrRewriteArgs2 {
    Rewriter2* rewriter;
    RewriterVarUsage2 obj, attrval;
    bool more_guards_after;

    bool out_success;

    SetattrRewriteArgs2(Rewriter2* rewriter, RewriterVarUsage2&& obj, RewriterVarUsage2&& attrval,
                        bool more_guards_after)
        : rewriter(rewriter), obj(std::move(obj)), attrval(std::move(attrval)), more_guards_after(more_guards_after),
          out_success(false) {}
};

struct LenRewriteArgs {
    Rewriter* rewriter;
    RewriterVar obj;
    bool out_success;
    RewriterVar out_rtn;

    int preferred_dest_reg;

    LenRewriteArgs(Rewriter* rewriter, const RewriterVar& obj)
        : rewriter(rewriter), obj(obj), out_success(false), preferred_dest_reg(-1) {}
};

struct CallRewriteArgs {
    Rewriter* rewriter;
    RewriterVar obj;
    RewriterVar arg1, arg2, arg3, args;
    bool out_success;
    RewriterVar out_rtn;
    bool func_guarded;
    bool args_guarded;

    int preferred_dest_reg;

    CallRewriteArgs(Rewriter* rewriter, const RewriterVar& obj)
        : rewriter(rewriter), obj(obj), out_success(false), func_guarded(false), args_guarded(false),
          preferred_dest_reg(-1) {}
};

struct BinopRewriteArgs {
    Rewriter* rewriter;
    RewriterVar lhs, rhs;
    bool out_success;
    RewriterVar out_rtn;

    BinopRewriteArgs(Rewriter* rewriter, const RewriterVar& lhs, const RewriterVar& rhs)
        : rewriter(rewriter), lhs(lhs), rhs(rhs), out_success(false) {}
};

struct CompareRewriteArgs {
    Rewriter* rewriter;
    RewriterVar lhs, rhs;
    bool out_success;
    RewriterVar out_rtn;

    CompareRewriteArgs(Rewriter* rewriter, const RewriterVar& lhs, const RewriterVar& rhs)
        : rewriter(rewriter), lhs(lhs), rhs(rhs), out_success(false) {}
};

Box* runtimeCallInternal(Box* obj, CallRewriteArgs* rewrite_args, ArgPassSpec argspec, Box* arg1, Box* arg2, Box* arg3,
                         Box** args, const std::vector<const std::string*>* keyword_names);
static Box* (*runtimeCallInternal0)(Box*, CallRewriteArgs*, ArgPassSpec)
    = (Box * (*)(Box*, CallRewriteArgs*, ArgPassSpec))runtimeCallInternal;
static Box* (*runtimeCallInternal1)(Box*, CallRewriteArgs*, ArgPassSpec, Box*)
    = (Box * (*)(Box*, CallRewriteArgs*, ArgPassSpec, Box*))runtimeCallInternal;
static Box* (*runtimeCallInternal2)(Box*, CallRewriteArgs*, ArgPassSpec, Box*, Box*)
    = (Box * (*)(Box*, CallRewriteArgs*, ArgPassSpec, Box*, Box*))runtimeCallInternal;
static Box* (*runtimeCallInternal3)(Box*, CallRewriteArgs*, ArgPassSpec, Box*, Box*, Box*)
    = (Box * (*)(Box*, CallRewriteArgs*, ArgPassSpec, Box*, Box*, Box*))runtimeCallInternal;

static Box* (*typeCallInternal1)(BoxedFunction*, CallRewriteArgs* rewrite_args, ArgPassSpec argspec, Box*)
    = (Box * (*)(BoxedFunction*, CallRewriteArgs*, ArgPassSpec, Box*))typeCallInternal;
static Box* (*typeCallInternal2)(BoxedFunction*, CallRewriteArgs* rewrite_args, ArgPassSpec argspec, Box*, Box*)
    = (Box * (*)(BoxedFunction*, CallRewriteArgs*, ArgPassSpec, Box*, Box*))typeCallInternal;
static Box* (*typeCallInternal3)(BoxedFunction*, CallRewriteArgs* rewrite_args, ArgPassSpec argspec, Box*, Box*, Box*)
    = (Box * (*)(BoxedFunction*, CallRewriteArgs*, ArgPassSpec, Box*, Box*, Box*))typeCallInternal;

bool checkClass(LookupScope scope) {
    return (scope & CLASS_ONLY) != 0;
}
bool checkInst(LookupScope scope) {
    return (scope & INST_ONLY) != 0;
}
static Box* (*callattrInternal0)(Box*, const std::string*, LookupScope, CallRewriteArgs*, ArgPassSpec)
    = (Box * (*)(Box*, const std::string*, LookupScope, CallRewriteArgs*, ArgPassSpec))callattrInternal;
static Box* (*callattrInternal1)(Box*, const std::string*, LookupScope, CallRewriteArgs*, ArgPassSpec, Box*)
    = (Box * (*)(Box*, const std::string*, LookupScope, CallRewriteArgs*, ArgPassSpec, Box*))callattrInternal;
static Box* (*callattrInternal2)(Box*, const std::string*, LookupScope, CallRewriteArgs*, ArgPassSpec, Box*, Box*)
    = (Box * (*)(Box*, const std::string*, LookupScope, CallRewriteArgs*, ArgPassSpec, Box*, Box*))callattrInternal;
static Box* (*callattrInternal3)(Box*, const std::string*, LookupScope, CallRewriteArgs*, ArgPassSpec, Box*, Box*, Box*)
    = (Box
       * (*)(Box*, const std::string*, LookupScope, CallRewriteArgs*, ArgPassSpec, Box*, Box*, Box*))callattrInternal;

size_t PyHasher::operator()(Box* b) const {
    if (b->cls == str_cls) {
        std::hash<std::string> H;
        return H(static_cast<BoxedString*>(b)->s);
    }

    BoxedInt* i = hash(b);
    assert(sizeof(size_t) == sizeof(i->n));
    size_t rtn = i->n;
    return rtn;
}

bool PyEq::operator()(Box* lhs, Box* rhs) const {
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

bool PyLt::operator()(Box* lhs, Box* rhs) const {
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

extern "C" bool isSubclass(BoxedClass* child, BoxedClass* parent) {
    // TODO the class is allowed to override this using __subclasscheck__
    while (child) {
        if (child == parent)
            return true;
        child = child->base;
    }
    return false;
}

extern "C" void assertFail(BoxedModule* inModule, Box* msg) {
    if (msg) {
        BoxedString* tostr = str(msg);
        raiseExcHelper(AssertionError, "%s", tostr->s.c_str());
    } else {
        raiseExcHelper(AssertionError, NULL);
    }
}

extern "C" void assertNameDefined(bool b, const char* name) {
    if (!b) {
        raiseExcHelper(UnboundLocalError, "local variable '%s' referenced before assignment", name);
    }
}

extern "C" void raiseAttributeErrorStr(const char* typeName, const char* attr) {
    raiseExcHelper(AttributeError, "'%s' object has no attribute '%s'", typeName, attr);
}

extern "C" void raiseAttributeError(Box* obj, const char* attr) {
    if (obj->cls == type_cls) {
        // Slightly different error message:
        raiseExcHelper(AttributeError, "type object '%s' has no attribute '%s'",
                       getNameOfClass(static_cast<BoxedClass*>(obj))->c_str(), attr);
    } else {
        raiseAttributeErrorStr(getTypeName(obj)->c_str(), attr);
    }
}

extern "C" void raiseNotIterableError(const char* typeName) {
    raiseExcHelper(TypeError, "'%s' object is not iterable", typeName);
}

extern "C" void checkUnpackingLength(i64 expected, i64 given) {
    if (given == expected)
        return;

    if (given > expected)
        raiseExcHelper(ValueError, "too many values to unpack");
    else {
        if (given == 1)
            raiseExcHelper(ValueError, "need more than %ld value to unpack", given);
        else
            raiseExcHelper(ValueError, "need more than %ld values to unpack", given);
    }
}

BoxedClass::BoxedClass(BoxedClass* base, int attrs_offset, int instance_size, bool is_user_defined)
    : Box(&type_flavor, type_cls), base(base), attrs_offset(attrs_offset), instance_size(instance_size),
      is_constant(false), is_user_defined(is_user_defined) {

    if (!base) {
        assert(object_cls == nullptr);
        // we're constructing 'object'
        // Will have to add __base__ = None later
    } else {
        assert(object_cls);
        if (base->attrs_offset)
            RELEASE_ASSERT(attrs_offset == base->attrs_offset, "");
        assert(instance_size >= base->instance_size);
    }

    if (base && cls && str_cls)
        giveAttr("__base__", base);

    assert(instance_size % sizeof(void*) == 0); // Not critical I suppose, but probably signals a bug
    if (attrs_offset) {
        assert(instance_size >= attrs_offset + sizeof(HCAttrs));
        assert(attrs_offset % sizeof(void*) == 0); // Not critical I suppose, but probably signals a bug
    }

    if (!is_user_defined)
        gc::registerStaticRootObj(this);
}

extern "C" const std::string* getNameOfClass(BoxedClass* cls) {
    Box* b = cls->getattr("__name__");
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

Box::Box(const ObjectFlavor* flavor, BoxedClass* cls) : GCObject(flavor), cls(cls) {
    // if (TRACK_ALLOCATIONS) {
    // int id = Stats::getStatId("allocated_" + *getNameOfClass(c));
    // Stats::log(id);
    //}

    // the only way cls should be NULL is if we're creating the type_cls
    // object itself:
    if (cls == NULL) {
        assert(type_cls == NULL);
    } else {
    }
}


HCAttrs* Box::getAttrsPtr() {
    assert(cls->instancesHaveAttrs());

    char* p = reinterpret_cast<char*>(this);
    p += cls->attrs_offset;
    return reinterpret_cast<HCAttrs*>(p);
}

Box* Box::getattr(const std::string& attr, GetattrRewriteArgs* rewrite_args, GetattrRewriteArgs2* rewrite_args2) {
    // Have to guard on the memory layout of this object.
    // Right now, guard on the specific Python-class, which in turn
    // specifies the C structure.
    // In the future, we could create another field (the flavor?)
    // that also specifies the structure and can include multiple
    // classes.
    // Only matters if we end up getting multiple classes with the same
    // structure (ex user class) and the same hidden classes, because
    // otherwise the guard will fail anyway.;
    if (rewrite_args) {
        rewrite_args->out_success = true;
        rewrite_args->obj.addAttrGuard(BOX_CLS_OFFSET, (intptr_t)cls);
    }
    if (rewrite_args2) {
        rewrite_args2->out_success = true;
        rewrite_args2->obj.addAttrGuard(BOX_CLS_OFFSET, (intptr_t)cls);
    }

    if (!cls->instancesHaveAttrs())
        return NULL;

    HCAttrs* attrs = getAttrsPtr();
    HiddenClass* hcls = attrs->hcls;

    if (rewrite_args) {
        if (!rewrite_args->obj_hcls_guarded)
            rewrite_args->obj.addAttrGuard(cls->attrs_offset + HCATTRS_HCLS_OFFSET, (intptr_t)hcls);
    }

    if (rewrite_args2) {
        if (!rewrite_args2->obj_hcls_guarded)
            rewrite_args2->obj.addAttrGuard(cls->attrs_offset + HCATTRS_HCLS_OFFSET, (intptr_t)hcls);
    }

    int offset = hcls->getOffset(attr);
    if (offset == -1)
        return NULL;

    if (rewrite_args) {
        // TODO using the output register as the temporary makes register allocation easier
        // since we don't need to clobber a register, but does it make the code slower?
        // int temp_reg = -2;
        // if (rewrite_args->preferred_dest_reg == -2)
        // temp_reg = -3;
        int temp_reg = rewrite_args->preferred_dest_reg;

        RewriterVar attrs = rewrite_args->obj.getAttr(cls->attrs_offset + HCATTRS_ATTRS_OFFSET, temp_reg);
        rewrite_args->out_rtn
            = attrs.getAttr(offset * sizeof(Box*) + ATTRLIST_ATTRS_OFFSET, rewrite_args->preferred_dest_reg);

        rewrite_args->rewriter->addDependenceOn(cls->dependent_icgetattrs);
    }

    if (rewrite_args2) {
        if (!rewrite_args2->more_guards_after)
            rewrite_args2->rewriter->setDoneGuarding();

        RewriterVarUsage2 attrs
            = rewrite_args2->obj.getAttr(cls->attrs_offset + HCATTRS_ATTRS_OFFSET, RewriterVarUsage2::Kill);
        rewrite_args2->out_rtn = attrs.getAttr(offset * sizeof(Box*) + ATTRLIST_ATTRS_OFFSET, RewriterVarUsage2::Kill,
                                               rewrite_args2->destination);
    }

    Box* rtn = attrs->attr_list->attrs[offset];
    return rtn;
}

// TODO should centralize all of these:
static const std::string _call_str("__call__"), _new_str("__new__"), _init_str("__init__");

void Box::setattr(const std::string& attr, Box* val, SetattrRewriteArgs2* rewrite_args2) {
    assert(cls->instancesHaveAttrs());

    // Have to guard on the memory layout of this object.
    // Right now, guard on the specific Python-class, which in turn
    // specifies the C structure.
    // In the future, we could create another field (the flavor?)
    // that also specifies the structure and can include multiple
    // classes.
    // Only matters if we end up getting multiple classes with the same
    // structure (ex user class) and the same hidden classes, because
    // otherwise the guard will fail anyway.;
    if (rewrite_args2)
        rewrite_args2->obj.addAttrGuard(BOX_CLS_OFFSET, (intptr_t)cls);


    static const std::string none_str("None");
    static const std::string getattr_str("__getattr__");
    static const std::string getattribute_str("__getattribute__");

    RELEASE_ASSERT(attr != none_str || this == builtins_module, "can't assign to None");

    if (isSubclass(this->cls, type_cls)) {
        BoxedClass* self = static_cast<BoxedClass*>(this);

        if (attr == getattr_str || attr == getattribute_str) {
            // Will have to embed the clear in the IC, so just disable the patching for now:
            rewrite_args2 = NULL;

            // TODO should put this clearing behavior somewhere else, since there are probably more
            // cases in which we want to do it.
            self->dependent_icgetattrs.invalidateAll();
        }

        if (attr == "__base__" && getattr("__base__")) {
            raiseExcHelper(TypeError, "readonly attribute");
        }
    }

    HCAttrs* attrs = getAttrsPtr();
    HiddenClass* hcls = attrs->hcls;
    int numattrs = hcls->attr_offsets.size();

    int offset = hcls->getOffset(attr);

    if (rewrite_args2) {
        rewrite_args2->obj.addAttrGuard(cls->attrs_offset + HCATTRS_HCLS_OFFSET, (intptr_t)hcls);

        if (!rewrite_args2->more_guards_after)
            rewrite_args2->rewriter->setDoneGuarding();
        // rewrite_args2->rewriter->addDecision(offset == -1 ? 1 : 0);
    }

    if (offset >= 0) {
        assert(offset < numattrs);
        Box* prev = attrs->attr_list->attrs[offset];
        attrs->attr_list->attrs[offset] = val;

        if (rewrite_args2) {

            RewriterVarUsage2 r_hattrs = rewrite_args2->obj.getAttr(cls->attrs_offset + HCATTRS_ATTRS_OFFSET,
                                                                    RewriterVarUsage2::Kill, Location::any());

            r_hattrs.setAttr(offset * sizeof(Box*) + ATTRLIST_ATTRS_OFFSET, std::move(rewrite_args2->attrval));
            r_hattrs.setDoneUsing();

            rewrite_args2->out_success = true;
        }

        return;
    }

    assert(offset == -1);
    HiddenClass* new_hcls = hcls->getOrMakeChild(attr);

    // TODO need to make sure we don't need to rearrange the attributes
    assert(new_hcls->attr_offsets[attr] == numattrs);
#ifndef NDEBUG
    for (const auto& p : hcls->attr_offsets) {
        assert(new_hcls->attr_offsets[p.first] == p.second);
    }
#endif



    RewriterVar r_new_array;
    RewriterVarUsage2 r_new_array2(RewriterVarUsage2::empty());
    int new_size = sizeof(HCAttrs::AttrList) + sizeof(Box*) * (numattrs + 1);
    if (numattrs == 0) {
        attrs->attr_list = (HCAttrs::AttrList*)rt_alloc(new_size);
        attrs->attr_list->gc_header.kind_id = untracked_kind.kind_id;
        if (rewrite_args2) {
            RewriterVarUsage2 r_newsize = rewrite_args2->rewriter->loadConst(new_size, Location::forArg(0));
            r_new_array2 = rewrite_args2->rewriter->call(false, (void*)rt_alloc, std::move(r_newsize));
            RewriterVarUsage2 r_flavor = rewrite_args2->rewriter->loadConst((int64_t)untracked_kind.kind_id);
            r_new_array2.setAttr(ATTRLIST_KIND_OFFSET, std::move(r_flavor));
        }
    } else {
        attrs->attr_list = (HCAttrs::AttrList*)rt_realloc(attrs->attr_list, new_size);
        if (rewrite_args2) {
            RewriterVarUsage2 r_oldarray = rewrite_args2->obj.getAttr(cls->attrs_offset + HCATTRS_ATTRS_OFFSET,
                                                                      RewriterVarUsage2::NoKill, Location::forArg(0));
            RewriterVarUsage2 r_newsize = rewrite_args2->rewriter->loadConst(new_size, Location::forArg(1));
            r_new_array2
                = rewrite_args2->rewriter->call(false, (void*)rt_realloc, std::move(r_oldarray), std::move(r_newsize));
        }
    }
    // Don't set the new hcls until after we do the allocation for the new attr_list;
    // that allocation can cause a collection, and we want the collector to always
    // see a consistent state between the hcls and the attr_list
    attrs->hcls = new_hcls;

    if (rewrite_args2) {
        r_new_array2.setAttr(numattrs * sizeof(Box*) + ATTRLIST_ATTRS_OFFSET, std::move(rewrite_args2->attrval));
        rewrite_args2->obj.setAttr(cls->attrs_offset + HCATTRS_ATTRS_OFFSET, std::move(r_new_array2));

        RewriterVarUsage2 r_hcls = rewrite_args2->rewriter->loadConst((intptr_t)new_hcls);
        rewrite_args2->obj.setAttr(cls->attrs_offset + HCATTRS_HCLS_OFFSET, std::move(r_hcls));
        rewrite_args2->obj.setDoneUsing();

        rewrite_args2->out_success = true;
    }
    attrs->attr_list->attrs[numattrs] = val;
}

static Box* _handleClsAttr(Box* obj, Box* attr) {
    if (attr->cls == function_cls) {
        Box* rtn = boxInstanceMethod(obj, attr);
        return rtn;
    }
    if (attr->cls == member_cls) {
        BoxedMemberDescriptor* member_desc = static_cast<BoxedMemberDescriptor*>(attr);
        switch (member_desc->type) {
            case BoxedMemberDescriptor::OBJECT: {
                assert(member_desc->offset % sizeof(Box*) == 0);
                Box* rtn = reinterpret_cast<Box**>(obj)[member_desc->offset / sizeof(Box*)];
                // be careful about returning NULLs; I'm not sure what the correct behavior is here:
                RELEASE_ASSERT(rtn, "");
                return rtn;
            }
            default:
                RELEASE_ASSERT(0, "%d", member_desc->type);
        }
        abort();
    }
    return attr;
}

Box* typeLookup(BoxedClass* cls, const std::string& attr, GetattrRewriteArgs* rewrite_args,
                GetattrRewriteArgs2* rewrite_args2) {
    Box* val;

    if (rewrite_args) {
        assert(!rewrite_args->out_success);

        val = cls->getattr(attr, rewrite_args, NULL);
        assert(rewrite_args->out_success);
        if (!val and cls->base) {
            rewrite_args->out_success = false;
            rewrite_args->obj = rewrite_args->obj.getAttr(offsetof(BoxedClass, base), rewrite_args->preferred_dest_reg);
            return typeLookup(cls->base, attr, rewrite_args, NULL);
        }
        return val;
    } else if (rewrite_args2) {
        assert(!rewrite_args2->out_success);

        val = cls->getattr(attr, NULL, rewrite_args2);
        assert(rewrite_args2->out_success);
        if (!val and cls->base) {
            rewrite_args2->out_success = false;
            rewrite_args2->obj = rewrite_args2->obj.getAttr(offsetof(BoxedClass, base), RewriterVarUsage2::Kill);
            return typeLookup(cls->base, attr, NULL, rewrite_args2);
        }
        return val;
    } else {
        val = cls->getattr(attr, NULL, NULL);
        if (!val and cls->base)
            return typeLookup(cls->base, attr, NULL, NULL);
        return val;
    }
}

Box* getclsattr_internal(Box* obj, const std::string& attr, GetattrRewriteArgs* rewrite_args,
                         GetattrRewriteArgs2* rewrite_args2) {
    Box* val;
    if (rewrite_args) {
        RewriterVar rcls = rewrite_args->obj.getAttr(BOX_CLS_OFFSET, 4);

        GetattrRewriteArgs sub_rewrite_args(rewrite_args->rewriter, rcls);
        sub_rewrite_args.preferred_dest_reg = 1;

        val = typeLookup(obj->cls, attr, &sub_rewrite_args, NULL);

        if (!sub_rewrite_args.out_success) {
            rewrite_args = NULL;
        } else {
            if (val)
                rewrite_args->out_rtn = sub_rewrite_args.out_rtn;
        }
    } else if (rewrite_args2) {
        RewriterVarUsage2 rcls = rewrite_args2->obj.getAttr(BOX_CLS_OFFSET, RewriterVarUsage2::NoKill);

        GetattrRewriteArgs2 sub_rewrite_args(rewrite_args2->rewriter, std::move(rcls), Location::forArg(1),
                                             rewrite_args2->more_guards_after);

        val = typeLookup(obj->cls, attr, NULL, &sub_rewrite_args);

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
        val = typeLookup(obj->cls, attr, NULL, NULL);
    }

    if (val == NULL) {
        if (rewrite_args)
            rewrite_args->out_success = true;
        if (rewrite_args2)
            rewrite_args2->out_success = true;
        return val;
    }

    if (rewrite_args) {
        // rewrite_args->rewriter->trap();
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
        RewriterVarUsage2 rrtn = rewrite_args2->rewriter->call(
            false, (void*)_handleClsAttr, std::move(rewrite_args2->obj), std::move(rewrite_args2->out_rtn));
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
    std::unique_ptr<Rewriter2> rewriter(
        Rewriter2::createRewriter(__builtin_extract_return_addr(__builtin_return_address(0)), 2, "getclsattr"));

    if (rewriter.get()) {
        // rewriter->trap();
        GetattrRewriteArgs2 rewrite_args(rewriter.get(), rewriter->getArg(0), rewriter->getReturnDestination(), false);
        gotten = getclsattr_internal(obj, attr, NULL, &rewrite_args);

        if (rewrite_args.out_success && gotten) {
            rewriter->commitReturning(std::move(rewrite_args.out_rtn));
        }
#endif
}
else {
    gotten = getclsattr_internal(obj, attr, NULL, NULL);
}
RELEASE_ASSERT(gotten, "%s:%s", getTypeName(obj)->c_str(), attr);

return gotten;
}

static Box* (*runtimeCall0)(Box*, ArgPassSpec) = (Box * (*)(Box*, ArgPassSpec))runtimeCall;
static Box* (*runtimeCall1)(Box*, ArgPassSpec, Box*) = (Box * (*)(Box*, ArgPassSpec, Box*))runtimeCall;
static Box* (*runtimeCall2)(Box*, ArgPassSpec, Box*, Box*) = (Box * (*)(Box*, ArgPassSpec, Box*, Box*))runtimeCall;
static Box* (*runtimeCall3)(Box*, ArgPassSpec, Box*, Box*, Box*)
    = (Box * (*)(Box*, ArgPassSpec, Box*, Box*, Box*))runtimeCall;

Box* getattr_internal(Box* obj, const std::string& attr, bool check_cls, bool allow_custom,
                      GetattrRewriteArgs* rewrite_args, GetattrRewriteArgs2* rewrite_args2) {
    if (allow_custom) {
        // Don't need to pass icentry args, since we special-case __getattribtue__ and __getattr__ to use
        // invalidation rather than guards
        Box* getattribute = getclsattr_internal(obj, "__getattribute__", NULL, NULL);
        if (getattribute) {
            // TODO this is a good candidate for interning?
            Box* boxstr = boxString(attr);
            Box* rtn = runtimeCall1(getattribute, ArgPassSpec(1), boxstr);
            return rtn;
        }

        if (rewrite_args) {
            rewrite_args->rewriter->addDependenceOn(obj->cls->dependent_icgetattrs);
        }
        if (rewrite_args2) {
            rewrite_args2->rewriter->addDependenceOn(obj->cls->dependent_icgetattrs);
        }
    }

    if (obj->cls == type_cls) {
        Box* val = typeLookup(static_cast<BoxedClass*>(obj), attr, rewrite_args, rewrite_args2);
        if (val)
            return val;

        // reset these since we reused them:
        if (rewrite_args)
            rewrite_args->out_success = false;
        if (rewrite_args2)
            rewrite_args2->out_success = false;
    } else {
        Box* val = NULL;
        if (rewrite_args) {
            GetattrRewriteArgs hrewrite_args(rewrite_args->rewriter, rewrite_args->obj);
            hrewrite_args.preferred_dest_reg = rewrite_args->preferred_dest_reg;
            val = obj->getattr(attr, &hrewrite_args, NULL);

            if (hrewrite_args.out_success) {
                if (val)
                    rewrite_args->out_rtn = hrewrite_args.out_rtn;
            } else {
                rewrite_args = NULL;
            }
        } else if (rewrite_args2) {
            GetattrRewriteArgs2 hrewrite_args(rewrite_args2->rewriter, std::move(rewrite_args2->obj),
                                              rewrite_args2->destination, rewrite_args2->more_guards_after);
            val = obj->getattr(attr, NULL, &hrewrite_args);

            if (hrewrite_args.out_success) {
                if (val)
                    rewrite_args2->out_rtn = std::move(hrewrite_args.out_rtn);
                else
                    rewrite_args2->obj = std::move(hrewrite_args.obj);
            } else {
                rewrite_args2 = NULL;
            }
        } else {
            val = obj->getattr(attr, NULL, NULL);
        }

        if (val) {
            if (rewrite_args)
                rewrite_args->out_success = true;
            if (rewrite_args2)
                rewrite_args2->out_success = true;
            return val;
        }
    }


    // TODO closures should get their own treatment, but now just piggy-back on the
    // normal hidden-class IC logic.
    // Can do better since we don't need to guard on the cls (always going to be closure)
    if (obj->cls == closure_cls) {
        BoxedClosure* closure = static_cast<BoxedClosure*>(obj);
        if (closure->parent) {
            if (rewrite_args) {
                rewrite_args->obj = rewrite_args->obj.getAttr(offsetof(BoxedClosure, parent), -1);
            }
            if (rewrite_args2) {
                rewrite_args2->obj
                    = rewrite_args2->obj.getAttr(offsetof(BoxedClosure, parent), RewriterVarUsage2::Kill);
            }
            return getattr_internal(closure->parent, attr, false, false, rewrite_args, rewrite_args2);
        }
        raiseExcHelper(NameError, "free variable '%s' referenced before assignment in enclosing scope", attr.c_str());
    }


    if (allow_custom) {
        // Don't need to pass icentry args, since we special-case __getattribtue__ and __getattr__ to use
        // invalidation rather than guards
        Box* getattr = getclsattr_internal(obj, "__getattr__", NULL, NULL);
        if (getattr) {
            Box* boxstr = boxString(attr);
            Box* rtn = runtimeCall1(getattr, ArgPassSpec(1), boxstr);
            return rtn;
        }

        if (rewrite_args) {
            rewrite_args->rewriter->addDependenceOn(obj->cls->dependent_icgetattrs);
        }
        if (rewrite_args2) {
            rewrite_args2->rewriter->addDependenceOn(obj->cls->dependent_icgetattrs);
        }
    }

    Box* rtn = NULL;
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
            GetattrRewriteArgs2 crewrite_args(rewrite_args2->rewriter, std::move(rewrite_args2->obj),
                                              rewrite_args2->destination, rewrite_args2->more_guards_after);
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
    if (rewrite_args)
        rewrite_args->out_success = true;
    if (rewrite_args2)
        rewrite_args2->out_success = true;

    return rtn;
}

extern "C" Box* getattr(Box* obj, const char* attr) {
    static StatCounter slowpath_getattr("slowpath_getattr");
    slowpath_getattr.log();

    if (VERBOSITY() >= 2) {
#if !DISABLE_STATS
        std::string per_name_stat_name = "getattr__" + std::string(attr);
        int id = Stats::getStatId(per_name_stat_name);
        Stats::log(id);
#endif
    }

    std::unique_ptr<Rewriter2> rewriter(
        Rewriter2::createRewriter(__builtin_extract_return_addr(__builtin_return_address(0)), 2, "getattr"));

    Box* val;
    if (rewriter.get()) {
        // rewriter->trap();
        Location dest;
        TypeRecorder* recorder = rewriter->getTypeRecorder();
        if (recorder)
            dest = Location::forArg(1);
        else
            dest = rewriter->getReturnDestination();
        GetattrRewriteArgs2 rewrite_args(rewriter.get(), rewriter->getArg(0), dest, false);
        val = getattr_internal(obj, attr, true, true, NULL, &rewrite_args);

        if (rewrite_args.out_success && val) {
            if (recorder) {
                RewriterVarUsage2 record_rtn = rewriter->call(
                    false, (void*)recordType, rewriter->loadConst((intptr_t)recorder, Location::forArg(0)),
                    std::move(rewrite_args.out_rtn));
                rewriter->commitReturning(std::move(record_rtn));

                recordType(recorder, val);
            } else {
                rewriter->commitReturning(std::move(rewrite_args.out_rtn));
            }
        } else {
            rewrite_args.obj.setDoneUsing();
        }
    } else {
        val = getattr_internal(obj, attr, true, true, NULL, NULL);
    }

    if (val) {
        return val;
    }
    raiseAttributeError(obj, attr);
}

extern "C" void setattr(Box* obj, const char* attr, Box* attr_val) {
    assert(strcmp(attr, "__class__") != 0);

    static StatCounter slowpath_setattr("slowpath_setattr");
    slowpath_setattr.log();

    if (!obj->cls->instancesHaveAttrs()) {
        raiseAttributeError(obj, attr);
    }

    if (obj->cls == type_cls) {
        BoxedClass* cobj = static_cast<BoxedClass*>(obj);
        if (!isUserDefined(cobj)) {
            raiseExcHelper(TypeError, "can't set attributes of built-in/extension type '%s'",
                           getNameOfClass(cobj)->c_str());
        }
    }

    std::unique_ptr<Rewriter2> rewriter(
        Rewriter2::createRewriter(__builtin_extract_return_addr(__builtin_return_address(0)), 3, "setattr"));

    if (rewriter.get()) {
        // rewriter->trap();
        SetattrRewriteArgs2 rewrite_args(rewriter.get(), rewriter->getArg(0), rewriter->getArg(2), false);
        obj->setattr(attr, attr_val, &rewrite_args);
        if (rewrite_args.out_success) {
            rewriter->commit();
        } else {
            rewrite_args.obj.setDoneUsing();
            rewrite_args.attrval.setDoneUsing();
        }
    } else {
        obj->setattr(attr, attr_val, NULL);
    }
}

bool isUserDefined(BoxedClass* cls) {
    return cls->is_user_defined;
    // return cls->hasattrs && (cls != function_cls && cls != type_cls) && !cls->is_constant;
}

extern "C" bool nonzero(Box* obj) {
    static StatCounter slowpath_nonzero("slowpath_nonzero");

    std::unique_ptr<Rewriter> rewriter(
        Rewriter::createRewriter(__builtin_extract_return_addr(__builtin_return_address(0)), 1, 0, "nonzero"));

    if (rewriter.get()) {
        // rewriter->trap();
        rewriter->getArg(0).addAttrGuard(BOX_CLS_OFFSET, (intptr_t)obj->cls);
    }

    if (obj->cls == bool_cls) {
        if (rewriter.get()) {
            rewriter->getArg(0).getAttr(BOOL_B_OFFSET, -1);
            rewriter->commit();
        }

        BoxedBool* bool_obj = static_cast<BoxedBool*>(obj);
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

        BoxedInt* int_obj = static_cast<BoxedInt*>(obj);
        return int_obj->n != 0;
    } else if (obj->cls == float_cls) {
        if (rewriter.get()) {
            rewriter->call((void*)floatNonzeroUnboxed);
            rewriter->commit();
        }
        return static_cast<BoxedFloat*>(obj)->d != 0;
    } else if (obj->cls == none_cls) {
        return false;
    }

    // FIXME we have internal functions calling this method;
    // instead, we should break this out into an external and internal function.
    // slowpath_* counters are supposed to count external calls; putting it down
    // here gets a better representation of that.
    // TODO move internal callers to nonzeroInternal, and log *all* calls to nonzero
    slowpath_nonzero.log();

    // int id = Stats::getStatId("slowpath_nonzero_" + *getTypeName(obj));
    // Stats::log(id);

    Box* func = getclsattr_internal(obj, "__nonzero__", NULL, NULL);
    if (func == NULL) {
        RELEASE_ASSERT(isUserDefined(obj->cls), "%s.__nonzero__", getTypeName(obj)->c_str()); // TODO
        return true;
    }

    Box* r = runtimeCall0(func, ArgPassSpec(0));
    if (r->cls == bool_cls) {
        BoxedBool* b = static_cast<BoxedBool*>(r);
        bool rtn = b->b;
        return rtn;
    } else if (r->cls == int_cls) {
        BoxedInt* b = static_cast<BoxedInt*>(r);
        bool rtn = b->n != 0;
        return rtn;
    } else {
        raiseExcHelper(TypeError, "__nonzero__ should return bool or int, returned %s", getTypeName(r)->c_str());
    }
}

extern "C" BoxedString* str(Box* obj) {
    static StatCounter slowpath_str("slowpath_str");
    slowpath_str.log();

    if (obj->cls != str_cls) {
        Box* str = getclsattr_internal(obj, "__str__", NULL, NULL);
        if (str == NULL)
            str = getclsattr_internal(obj, "__repr__", NULL, NULL);

        if (str == NULL) {
            char buf[80];
            snprintf(buf, 80, "<%s object at %p>", getTypeName(obj)->c_str(), obj);
            return boxStrConstant(buf);
        } else {
            obj = runtimeCallInternal0(str, NULL, ArgPassSpec(0));
        }
    }
    if (obj->cls != str_cls) {
        fprintf(stderr, "__str__ did not return a string!\n");
        abort();
    }
    return static_cast<BoxedString*>(obj);
}

extern "C" Box* repr(Box* obj) {
    static StatCounter slowpath_repr("slowpath_repr");
    slowpath_repr.log();

    Box* repr = getclsattr_internal(obj, "__repr__", NULL, NULL);
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
        obj = runtimeCall0(repr, ArgPassSpec(0));
    }

    if (obj->cls != str_cls) {
        raiseExcHelper(TypeError, "__repr__ did not return a string!");
    }
    return static_cast<BoxedString*>(obj);
}

extern "C" BoxedString* reprOrNull(Box* obj) {
    try {
        Box* r = repr(obj);
        assert(r->cls == str_cls); // this should be checked by repr()
        return static_cast<BoxedString*>(r);
    } catch (Box* b) {
        return nullptr;
    }
}

extern "C" BoxedString* strOrNull(Box* obj) {
    try {
        BoxedString* r = str(obj);
        return static_cast<BoxedString*>(r);
    } catch (Box* b) {
        return nullptr;
    }
}

extern "C" bool isinstance(Box* obj, Box* cls, int64_t flags) {
    bool false_on_noncls = (flags & 0x1);

    if (!false_on_noncls) {
        assert(cls->cls == type_cls);
    } else {
        if (cls->cls != type_cls)
            return false;
    }

    BoxedClass* ccls = static_cast<BoxedClass*>(cls);

    // TODO the class is allowed to override this using __instancecheck__
    return isSubclass(obj->cls, ccls);
}

extern "C" BoxedInt* hash(Box* obj) {
    static StatCounter slowpath_hash("slowpath_hash");
    slowpath_hash.log();

    Box* hash = getclsattr_internal(obj, "__hash__", NULL, NULL);
    if (hash == NULL) {
        ASSERT(isUserDefined(obj->cls), "%s.__hash__", getTypeName(obj)->c_str());
        // TODO not the best way to handle this...
        return static_cast<BoxedInt*>(boxInt((i64)obj));
    }

    Box* rtn = runtimeCall0(hash, ArgPassSpec(0));
    if (rtn->cls != int_cls) {
        raiseExcHelper(TypeError, "an integer is required");
    }
    return static_cast<BoxedInt*>(rtn);
}

extern "C" BoxedInt* lenInternal(Box* obj, LenRewriteArgs* rewrite_args) {
    Box* rtn;
    static std::string attr_str("__len__");
    if (rewrite_args) {
        CallRewriteArgs crewrite_args(rewrite_args->rewriter, rewrite_args->obj);
        crewrite_args.preferred_dest_reg = rewrite_args->preferred_dest_reg;
        rtn = callattrInternal0(obj, &attr_str, CLASS_ONLY, &crewrite_args, ArgPassSpec(0));
        if (!crewrite_args.out_success)
            rewrite_args = NULL;
        else if (rtn)
            rewrite_args->out_rtn = crewrite_args.out_rtn;
    } else {
        rtn = callattrInternal0(obj, &attr_str, CLASS_ONLY, NULL, ArgPassSpec(0));
    }

    if (rtn == NULL) {
        raiseExcHelper(TypeError, "object of type '%s' has no len()", getTypeName(obj)->c_str());
    }

    if (rtn->cls != int_cls) {
        raiseExcHelper(TypeError, "an integer is required");
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

    std::unique_ptr<Rewriter> rewriter(
        Rewriter::createRewriter(__builtin_extract_return_addr(__builtin_return_address(0)), 1, 1, "unboxedLen"));

    BoxedInt* lobj;
    RewriterVar r_boxed;
    if (rewriter.get()) {
        // rewriter->trap();
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

extern "C" void print(Box* obj) {
    static StatCounter slowpath_print("slowpath_print");
    slowpath_print.log();

    BoxedString* strd = str(obj);
    printf("%s", strd->s.c_str());
}

extern "C" void dump(void* p) {
    printf("\n");
    bool is_gc = (gc::global_heap.getAllocationFromInteriorPointer(p) != NULL);
    if (!is_gc) {
        printf("non-gc memory\n");
        return;
    }

    GCObjectHeader* header = gc::headerFromObject(p);
    if (header->kind_id == hc_kind.kind_id) {
        printf("hcls object\n");
        return;
    }

    if (header->kind_id == untracked_kind.kind_id) {
        printf("untracked object\n");
        return;
    }

    if (header->kind_id == conservative_kind.kind_id) {
        printf("untracked object\n");
        return;
    }

    printf("Assuming it's a Box*\n");
    Box* b = (Box*)p;
    printf("Class: %s\n", getTypeName(b)->c_str());
    if (isSubclass(b->cls, type_cls)) {
        printf("Type name: %s\n", getNameOfClass(static_cast<BoxedClass*>(b))->c_str());
    }
}

// For rewriting purposes, this function assumes that nargs will be constant.
// That's probably fine for some uses (ex binops), but otherwise it should be guarded on beforehand.
extern "C" Box* callattrInternal(Box* obj, const std::string* attr, LookupScope scope, CallRewriteArgs* rewrite_args,
                                 ArgPassSpec argspec, Box* arg1, Box* arg2, Box* arg3, Box** args,
                                 const std::vector<const std::string*>* keyword_names) {
    int npassed_args = argspec.totalPassed();

    if (rewrite_args) {
        // if (VERBOSITY()) {
        // printf("callattrInternal: %d", rewrite_args->obj.getArgnum());
        // if (npassed_args >= 1) printf(" %d", rewrite_args->arg1.getArgnum());
        // if (npassed_args >= 2) printf(" %d", rewrite_args->arg2.getArgnum());
        // if (npassed_args >= 3) printf(" %d", rewrite_args->arg3.getArgnum());
        // if (npassed_args >= 4) printf(" %d", rewrite_args->args.getArgnum());
        // printf("\n");
        //}
        if (rewrite_args->obj.getArgnum() == -1) {
            // rewrite_args->rewriter->trap();
            rewrite_args->obj = rewrite_args->obj.move(-3);
        }
    }

    if (rewrite_args && !rewrite_args->args_guarded) {
        // TODO duplication with runtime_call
        // TODO should know which args don't need to be guarded, ex if we're guaranteed that they
        // already fit, either since the type inferencer could determine that,
        // or because they only need to fit into an UNKNOWN slot.

        if (npassed_args >= 1)
            rewrite_args->arg1.addAttrGuard(BOX_CLS_OFFSET, (intptr_t)arg1->cls);
        if (npassed_args >= 2)
            rewrite_args->arg2.addAttrGuard(BOX_CLS_OFFSET, (intptr_t)arg2->cls);
        // Have to move(-1) since the arg is (probably/maybe) on the stack;
        // TODO ideally would handle that case, but for now just do the move() which
        // it knows how to handle
        if (npassed_args >= 3)
            rewrite_args->arg3.move(-2).addAttrGuard(BOX_CLS_OFFSET, (intptr_t)arg3->cls);

        if (npassed_args > 3) {
            RewriterVar r_args = rewrite_args->args.move(-3);
            for (int i = 3; i < npassed_args; i++) {
                // TODO if there are a lot of args (>16), might be better to increment a pointer
                // rather index them directly?
                r_args.getAttr((i - 3) * sizeof(Box*), -2).addAttrGuard(BOX_CLS_OFFSET, (intptr_t)args[i - 3]->cls);
            }
        }
    }


    if (checkInst(scope)) {
        Box* inst_attr;
        RewriterVar r_instattr;
        if (rewrite_args) {
            GetattrRewriteArgs ga_rewrite_args(rewrite_args->rewriter, rewrite_args->obj);

            inst_attr = getattr_internal(obj, *attr, false, true, &ga_rewrite_args, NULL);

            if (!ga_rewrite_args.out_success)
                rewrite_args = NULL;
            else if (inst_attr)
                r_instattr = ga_rewrite_args.out_rtn;
        } else {
            inst_attr = getattr_internal(obj, *attr, false, true, NULL, NULL);
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

                rtn = runtimeCallInternal(inst_attr, rewrite_args, argspec, arg1, arg2, arg3, args, keyword_names);

                if (rewrite_args->out_success) {
                    r_instattr = rewrite_args->rewriter->pop(0);
                }
            } else {
                rtn = runtimeCallInternal(inst_attr, NULL, argspec, arg1, arg2, arg3, args, keyword_names);
            }

            if (!rtn) {
                raiseExcHelper(TypeError, "'%s' object is not callable", getTypeName(inst_attr)->c_str());
            }

            return rtn;
        }
    }

    Box* clsattr = NULL;
    RewriterVar r_clsattr;
    if (checkClass(scope)) {
        if (rewrite_args) {
            // rewrite_args->obj.push();
            RewriterVar r_cls = rewrite_args->obj.getAttr(BOX_CLS_OFFSET, -1);
            GetattrRewriteArgs ga_rewrite_args(rewrite_args->rewriter, r_cls);

            r_cls.assertValid();
            clsattr = typeLookup(obj->cls, *attr, &ga_rewrite_args, NULL);

            if (!ga_rewrite_args.out_success)
                rewrite_args = NULL;
            else if (clsattr)
                r_clsattr = ga_rewrite_args.out_rtn.move(-1);
        } else {
            clsattr = typeLookup(obj->cls, *attr, NULL, NULL);
        }
    }

    if (!clsattr) {
        if (rewrite_args)
            rewrite_args->out_success = true;
        return NULL;
    }

    if (clsattr->cls == function_cls) {
        if (rewrite_args) {
            r_clsattr.addGuard((int64_t)clsattr);
        }

        // TODO copy from runtimeCall
        // TODO these two branches could probably be folded together (the first one is becoming
        // a subset of the second)
        if (npassed_args <= 2) {
            Box* rtn;
            if (rewrite_args) {
                CallRewriteArgs srewrite_args(rewrite_args->rewriter, r_clsattr);
                srewrite_args.arg1 = rewrite_args->obj;

                // should be no-ops:
                if (npassed_args >= 1)
                    srewrite_args.arg2 = rewrite_args->arg1;
                if (npassed_args >= 2)
                    srewrite_args.arg3 = rewrite_args->arg2;

                srewrite_args.func_guarded = true;
                srewrite_args.args_guarded = true;
                r_clsattr.push();

                rtn = runtimeCallInternal(
                    clsattr, &srewrite_args,
                    ArgPassSpec(argspec.num_args + 1, argspec.num_keywords, argspec.has_starargs, argspec.has_kwargs),
                    obj, arg1, arg2, NULL, keyword_names);

                if (!srewrite_args.out_success) {
                    rewrite_args = NULL;
                } else {
                    r_clsattr = rewrite_args->rewriter->pop(0);
                    rewrite_args->out_rtn = srewrite_args.out_rtn;
                }
            } else {
                rtn = runtimeCallInternal(clsattr, NULL, ArgPassSpec(argspec.num_args + 1, argspec.num_keywords,
                                                                     argspec.has_starargs, argspec.has_kwargs),
                                          obj, arg1, arg2, NULL, keyword_names);
            }

            if (rewrite_args)
                rewrite_args->out_success = true;
            return rtn;
        } else {
            int alloca_size = sizeof(Box*) * (npassed_args + 1 - 3);

            Box** new_args = (Box**)alloca(alloca_size);
            new_args[0] = arg3;
            memcpy(new_args + 1, args, (npassed_args - 3) * sizeof(Box*));

            Box* rtn;
            if (rewrite_args) {
                const bool annotate = 0;
                if (annotate)
                    rewrite_args->rewriter->trap();
                // if (VERBOSITY()) printf("have to remunge: %d %d %d %d\n", rewrite_args->arg1.getArgnum(),
                // rewrite_args->arg2.getArgnum(), rewrite_args->arg3.getArgnum(), rewrite_args->args.getArgnum());
                // The above line seems to print one of:
                // 4 5 6 7
                // 2 3 4 5
                // Want to move them to
                // 1 2 X X

                // if (npassed_args >= 1) rewrite_args->arg1 = rewrite_args->arg1.move(1);
                // if (npassed_args >= 2) rewrite_args->arg2 = rewrite_args->arg2.move(2);
                // if (npassed_args >= 3) rewrite_args->arg3 = rewrite_args->arg3.move(4);
                // if (npassed_args >= 4) rewrite_args->args = rewrite_args->args.move(5);

                // There's nothing critical that these are in these registers,
                // just that the register assignments for the rest of this
                // section assume that this is true:
                // assert(rewrite_args->obj.getArgnum() == 0);
                assert(r_clsattr.getArgnum() == -1);

                int new_alloca_reg = -3;
                RewriterVar r_new_args = rewrite_args->rewriter->alloca_(alloca_size, new_alloca_reg);
                r_clsattr.push();

                if (rewrite_args->arg3.isInReg())
                    r_new_args.setAttr(0, rewrite_args->arg3, /* user_visible = */ false);
                else {
                    r_new_args.setAttr(0, rewrite_args->arg3.move(-2), /* user_visible = */ false);
                }

                // arg3 is now dead
                for (int i = 0; i < npassed_args - 3; i++) {
                    RewriterVar arg;
                    if (rewrite_args->args.isInReg())
                        arg = rewrite_args->args.getAttr(i * sizeof(Box*), -2);
                    else {
                        // TODO this is really bad:
                        arg = rewrite_args->args.move(-2).getAttr(i * sizeof(Box*), -2);
                    }
                    r_new_args.setAttr((i + 1) * sizeof(Box*), arg, /* user_visible = */ false);
                }
                // args is now dead

                CallRewriteArgs srewrite_args(rewrite_args->rewriter, r_clsattr);
                srewrite_args.arg1 = rewrite_args->obj;
                if (npassed_args >= 1)
                    srewrite_args.arg2 = rewrite_args->arg1;
                if (npassed_args >= 2)
                    srewrite_args.arg3 = rewrite_args->arg2;
                if (npassed_args >= 3)
                    srewrite_args.args = r_new_args;
                srewrite_args.args_guarded = true;
                srewrite_args.func_guarded = true;

                if (annotate)
                    rewrite_args->rewriter->annotate(0);
                rtn = runtimeCallInternal(
                    clsattr, &srewrite_args,
                    ArgPassSpec(argspec.num_args + 1, argspec.num_keywords, argspec.has_starargs, argspec.has_kwargs),
                    obj, arg1, arg2, new_args, keyword_names);
                if (annotate)
                    rewrite_args->rewriter->annotate(1);

                if (!srewrite_args.out_success)
                    rewrite_args = NULL;
                else {
                    r_clsattr = rewrite_args->rewriter->pop(0);
                    rewrite_args->out_rtn = srewrite_args.out_rtn;

                    // TODO should be a dealloca or smth
                    rewrite_args->rewriter->alloca_(-alloca_size, 0);
                    rewrite_args->out_success = true;
                }
                if (annotate)
                    rewrite_args->rewriter->annotate(2);
            } else {
                rtn = runtimeCallInternal(clsattr, NULL, ArgPassSpec(argspec.num_args + 1, argspec.num_keywords,
                                                                     argspec.has_starargs, argspec.has_kwargs),
                                          obj, arg1, arg2, new_args, keyword_names);
            }
            return rtn;
        }
    } else {
        Box* rtn;
        if (clsattr->cls != function_cls)
            rewrite_args = NULL;

        if (rewrite_args) {
            CallRewriteArgs srewrite_args(rewrite_args->rewriter, r_clsattr);
            if (npassed_args >= 1)
                srewrite_args.arg1 = rewrite_args->arg1;
            if (npassed_args >= 2)
                srewrite_args.arg2 = rewrite_args->arg2;
            if (npassed_args >= 3)
                srewrite_args.arg3 = rewrite_args->arg3;
            if (npassed_args >= 4)
                srewrite_args.args = rewrite_args->args;
            srewrite_args.args_guarded = true;

            rtn = runtimeCallInternal(clsattr, &srewrite_args, argspec, arg1, arg2, arg3, args, keyword_names);

            if (!srewrite_args.out_success)
                rewrite_args = NULL;
            else
                rewrite_args->out_rtn = srewrite_args.out_rtn;
        } else {
            rtn = runtimeCallInternal(clsattr, NULL, argspec, arg1, arg2, arg3, args, keyword_names);
        }

        if (!rtn) {
            raiseExcHelper(TypeError, "'%s' object is not callable", getTypeName(clsattr)->c_str());
        }

        if (rewrite_args)
            rewrite_args->out_success = true;
        return rtn;
    }
}

extern "C" Box* callattr(Box* obj, std::string* attr, bool clsonly, ArgPassSpec argspec, Box* arg1, Box* arg2,
                         Box* arg3, Box** args, const std::vector<const std::string*>* keyword_names) {
    int npassed_args = argspec.totalPassed();

    static StatCounter slowpath_callattr("slowpath_callattr");
    slowpath_callattr.log();

    assert(attr);

    int num_orig_args = 4 + std::min(4, npassed_args);
    if (argspec.num_keywords)
        num_orig_args++;
    std::unique_ptr<Rewriter> rewriter(Rewriter::createRewriter(
        __builtin_extract_return_addr(__builtin_return_address(0)), num_orig_args, 2, "callattr"));
    Box* rtn;

    LookupScope scope = clsonly ? CLASS_ONLY : CLASS_OR_INST;

    if (rewriter.get()) {
        // rewriter->trap();

        // TODO feel weird about doing this; it either isn't necessary
        // or this kind of thing is necessary in a lot more places
        // rewriter->getArg(3).addGuard(npassed_args);

        CallRewriteArgs rewrite_args(rewriter.get(), rewriter->getArg(0));
        if (npassed_args >= 1)
            rewrite_args.arg1 = rewriter->getArg(4);
        if (npassed_args >= 2)
            rewrite_args.arg2 = rewriter->getArg(5);
        if (npassed_args >= 3)
            rewrite_args.arg3 = rewriter->getArg(6);
        if (npassed_args >= 4)
            rewrite_args.args = rewriter->getArg(7);
        rtn = callattrInternal(obj, attr, scope, &rewrite_args, argspec, arg1, arg2, arg3, args, keyword_names);

        if (!rewrite_args.out_success) {
            rewriter.reset(NULL);
        } else if (rtn) {
            rewrite_args.out_rtn.move(-1);
        }
    } else {
        rtn = callattrInternal(obj, attr, scope, NULL, argspec, arg1, arg2, arg3, args, keyword_names);
    }

    if (rtn == NULL) {
        raiseAttributeError(obj, attr->c_str());
    }

    if (rewriter.get())
        rewriter->commit();
    return rtn;
}

static inline Box*& getArg(int idx, Box*& arg1, Box*& arg2, Box*& arg3, Box** args) {
    if (idx == 0)
        return arg1;
    if (idx == 1)
        return arg2;
    if (idx == 2)
        return arg3;
    return args[idx - 3];
}

static CompiledFunction* pickVersion(CLFunction* f, int num_output_args, Box* oarg1, Box* oarg2, Box* oarg3,
                                     Box** oargs) {
    LOCK_REGION(codegen_rwlock.asWrite());

    CompiledFunction* chosen_cf = NULL;
    for (CompiledFunction* cf : f->versions) {
        assert(cf->spec->arg_types.size() == num_output_args);

        if (cf->spec->rtn_type->llvmType() != UNKNOWN->llvmType())
            continue;

        bool works = true;
        for (int i = 0; i < num_output_args; i++) {
            Box* arg = getArg(i, oarg1, oarg2, oarg3, oargs);

            ConcreteCompilerType* t = cf->spec->arg_types[i];
            if ((arg && !t->isFitBy(arg->cls)) || (!arg && t != UNKNOWN)) {
                works = false;
                break;
            }
        }

        if (!works)
            continue;

        chosen_cf = cf;
        break;
    }

    if (chosen_cf == NULL) {
        if (f->source == NULL) {
            // TODO I don't think this should be happening any more?
            printf("Error: couldn't find suitable function version and no source to recompile!\n");
            abort();
        }

        std::vector<ConcreteCompilerType*> arg_types;
        for (int i = 0; i < num_output_args; i++) {
            Box* arg = getArg(i, oarg1, oarg2, oarg3, oargs);
            assert(arg); // only builtin functions can pass NULL args

            arg_types.push_back(typeFromClass(arg->cls));
        }
        FunctionSpecialization* spec = new FunctionSpecialization(UNKNOWN, arg_types);

        EffortLevel::EffortLevel new_effort = initialEffort();

        // this also pushes the new CompiledVersion to the back of the version list:
        chosen_cf = compileFunction(f, spec, new_effort, NULL);
    }

    return chosen_cf;
}

static void placeKeyword(const std::vector<AST_expr*>& arg_names, std::vector<bool>& params_filled,
                         const std::string& kw_name, Box* kw_val, Box*& oarg1, Box*& oarg2, Box*& oarg3, Box** oargs,
                         BoxedDict* okwargs) {
    assert(kw_val);

    bool found = false;
    for (int j = 0; j < arg_names.size(); j++) {
        AST_expr* e = arg_names[j];
        if (e->type != AST_TYPE::Name)
            continue;

        AST_Name* n = ast_cast<AST_Name>(e);
        if (n->id == kw_name) {
            if (params_filled[j]) {
                raiseExcHelper(TypeError, "<function>() got multiple values for keyword argument '%s'",
                               kw_name.c_str());
            }

            getArg(j, oarg1, oarg2, oarg3, oargs) = kw_val;
            params_filled[j] = true;

            found = true;
            break;
        }
    }

    if (!found) {
        if (okwargs) {
            okwargs->d[boxString(kw_name)] = kw_val;
        } else {
            raiseExcHelper(TypeError, "<function>() got an unexpected keyword argument '%s'", kw_name.c_str());
        }
    }
}

Box* callFunc(BoxedFunction* func, CallRewriteArgs* rewrite_args, ArgPassSpec argspec, Box* arg1, Box* arg2, Box* arg3,
              Box** args, const std::vector<const std::string*>* keyword_names) {
    /*
     * Procedure:
     * - First match up positional arguments; any extra go to varargs.  error if too many.
     * - Then apply keywords; any extra go to kwargs.  error if too many.
     * - Use defaults to fill in any missing
     * - error about missing parameters
     */

    static StatCounter slowpath_resolveclfunc("slowpath_callfunc");
    slowpath_resolveclfunc.log();

    CLFunction* f = func->f;
    FunctionList& versions = f->versions;

    int num_output_args = f->numReceivedArgs();
    int num_passed_args = argspec.totalPassed();

    BoxedClosure* closure = func->closure;

    if (argspec.has_starargs || argspec.has_kwargs || f->takes_kwargs)
        rewrite_args = NULL;

    // These could be handled:
    if (argspec.num_keywords)
        rewrite_args = NULL;

    // TODO Should we guard on the CLFunction or the BoxedFunction?
    // A single CLFunction could end up forming multiple BoxedFunctions, and we
    // could emit assembly that handles any of them.  But doing this involves some
    // extra indirection, and it's not clear if that's worth it, since it seems like
    // the common case will be functions only ever getting a single set of default arguments.
    bool guard_clfunc = false;
    assert(!guard_clfunc && "I think there are users that expect the boxedfunction to be guarded");

    RewriterVar r_defaults_array;
    if (rewrite_args) {
        assert(rewrite_args->args_guarded && "need to guard args here");

        if (!rewrite_args->func_guarded) {
            if (guard_clfunc) {
                rewrite_args->obj.addAttrGuard(offsetof(BoxedFunction, f), (intptr_t)f);
            } else {
                rewrite_args->obj.addGuard((intptr_t)func);
            }
        }
        if (guard_clfunc) {
            // Have to save the defaults array since the object itself will probably get overwritten:
            rewrite_args->obj = rewrite_args->obj.move(-2);
            r_defaults_array = rewrite_args->obj.getAttr(offsetof(BoxedFunction, defaults), -2);
        }
    }

    if (rewrite_args) {
        int closure_indicator = closure ? 1 : 0;

        if (num_passed_args >= 1)
            rewrite_args->arg1 = rewrite_args->arg1.move(0 + closure_indicator);
        if (num_passed_args >= 2)
            rewrite_args->arg2 = rewrite_args->arg2.move(1 + closure_indicator);
        if (num_passed_args >= 3)
            rewrite_args->arg3 = rewrite_args->arg3.move(2 + closure_indicator);
        if (num_passed_args >= 4)
            rewrite_args->args = rewrite_args->args.move(3 + closure_indicator);

        // TODO this kind of embedded reference needs to be tracked by the GC somehow?
        // Or maybe it's ok, since we've guarded on the function object?
        if (closure)
            rewrite_args->rewriter->loadConst(0, (intptr_t)closure);

        // We might have trouble if we have more output args than input args,
        // such as if we need more space to pass defaults.
        if (num_output_args > 3 && num_output_args > argspec.totalPassed()) {
            int arg_bytes_required = (num_output_args - 3) * sizeof(Box*);

            // Try to fit it in the scratch space
            // TODO it should be a separate arg scratch space, esp once we switch this to rewriter2
            if (arg_bytes_required > rewrite_args->rewriter->getScratchBytes()) {
                // if it doesn't fit, just bail.
                // TODO could try to rt_alloc some space maybe?
                rewrite_args = NULL;
            } else {
                // Otherwise, use the scratch space for the arguments:
                RewriterVar rbp = rewrite_args->rewriter->getRbp();

                // This could just be a single LEA:
                RewriterVar new_args = rbp.move(-1);
                int rbp_offset = rewrite_args->rewriter->getScratchRbpOffset();
                new_args = new_args.add(rbp_offset);

                for (int i = 3; i < argspec.totalPassed(); i++) {
                    int offset = (i - 3) * sizeof(Box*);
                    RewriterVar old_arg = rewrite_args->args.getAttr(offset, -2);
                    new_args.setAttr(offset, old_arg);
                }
                rewrite_args->args = new_args.move(3);
            }
        }
    }


    std::vector<Box*> varargs;
    if (argspec.has_starargs) {
        Box* given_varargs = getArg(argspec.num_args + argspec.num_keywords, arg1, arg2, arg3, args);
        for (Box* e : given_varargs->pyElements()) {
            varargs.push_back(e);
        }
    }

    // The "output" args that we will pass to the called function:
    Box* oarg1 = NULL, *oarg2 = NULL, *oarg3 = NULL;
    Box** oargs = NULL;

    if (num_output_args > 3)
        oargs = (Box**)alloca((num_output_args - 3) * sizeof(Box*));

    ////
    // First, match up positional parameters to positional/varargs:
    int positional_to_positional = std::min((int)argspec.num_args, f->num_args);
    for (int i = 0; i < positional_to_positional; i++) {
        getArg(i, oarg1, oarg2, oarg3, oargs) = getArg(i, arg1, arg2, arg3, args);

        // we already moved the positional args into position
    }

    int varargs_to_positional = std::min((int)varargs.size(), f->num_args - positional_to_positional);
    for (int i = 0; i < varargs_to_positional; i++) {
        assert(!rewrite_args && "would need to be handled here");
        getArg(i + positional_to_positional, oarg1, oarg2, oarg3, oargs) = varargs[i];
    }

    std::vector<bool> params_filled(argspec.num_args, false);
    for (int i = 0; i < positional_to_positional + varargs_to_positional; i++) {
        params_filled[i] = true;
    }



    std::vector<Box*, StlCompatAllocator<Box*> > unused_positional;
    for (int i = positional_to_positional; i < argspec.num_args; i++) {
        rewrite_args = NULL;
        unused_positional.push_back(getArg(i, arg1, arg2, arg3, args));
    }
    for (int i = varargs_to_positional; i < varargs.size(); i++) {
        rewrite_args = NULL;
        unused_positional.push_back(varargs[i]);
    }

    if (f->takes_varargs) {
        int varargs_idx = f->num_args;
        if (rewrite_args) {
            assert(!unused_positional.size());
            rewrite_args->rewriter->loadConst(varargs_idx, (intptr_t)EmptyTuple);
        }

        Box* ovarargs = new BoxedTuple(std::move(unused_positional));
        getArg(varargs_idx, oarg1, oarg2, oarg3, oargs) = ovarargs;
    } else if (unused_positional.size()) {
        raiseExcHelper(TypeError, "<function>() takes at most %d argument%s (%d given)", f->num_args,
                       (f->num_args == 1 ? "" : "s"), argspec.num_args + argspec.num_keywords + varargs.size());
    }

    ////
    // Second, apply any keywords:

    BoxedDict* okwargs = NULL;
    if (f->takes_kwargs) {
        assert(!rewrite_args && "would need to be handled here");
        okwargs = new BoxedDict();
        getArg(f->num_args + (f->takes_varargs ? 1 : 0), oarg1, oarg2, oarg3, oargs) = okwargs;
    }

    const std::vector<AST_expr*>* arg_names = f->source ? f->source->arg_names.args : NULL;
    if (arg_names == nullptr && argspec.num_keywords) {
        raiseExcHelper(TypeError, "<function @%p>() doesn't take keyword arguments", f->versions[0]->code);
    }

    if (argspec.num_keywords)
        assert(argspec.num_keywords == keyword_names->size());

    for (int i = 0; i < argspec.num_keywords; i++) {
        assert(!rewrite_args && "would need to be handled here");
        assert(arg_names);

        int arg_idx = i + argspec.num_args;
        Box* kw_val = getArg(arg_idx, arg1, arg2, arg3, args);

        placeKeyword(*arg_names, params_filled, *(*keyword_names)[i], kw_val, oarg1, oarg2, oarg3, oargs, okwargs);
    }

    if (argspec.has_kwargs) {
        assert(!rewrite_args && "would need to be handled here");
        assert(arg_names);

        Box* kwargs
            = getArg(argspec.num_args + argspec.num_keywords + (argspec.has_starargs ? 1 : 0), arg1, arg2, arg3, args);
        RELEASE_ASSERT(kwargs->cls == dict_cls, "haven't implemented this for non-dicts");

        BoxedDict* d_kwargs = static_cast<BoxedDict*>(kwargs);
        for (auto& p : d_kwargs->d) {
            if (p.first->cls != str_cls)
                raiseExcHelper(TypeError, "<function>() keywords must be strings");

            BoxedString* s = static_cast<BoxedString*>(p.first);
            placeKeyword(*arg_names, params_filled, s->s, p.second, oarg1, oarg2, oarg3, oargs, okwargs);
        }
    }



    // Fill with defaults:

    for (int i = 0; i < f->num_args - f->num_defaults; i++) {
        if (params_filled[i])
            continue;
        // TODO not right error message
        raiseExcHelper(TypeError, "<function>() did not get a value for positional argument %d", i);
    }

    for (int i = f->num_args - f->num_defaults; i < f->num_args; i++) {
        if (params_filled[i])
            continue;

        int default_idx = i + f->num_defaults - f->num_args;
        Box* default_obj = func->defaults->elts[default_idx];

        if (rewrite_args) {
            int offset = offsetof(std::remove_pointer<decltype(BoxedFunction::defaults)>::type, elts)
                         + sizeof(Box*) * default_idx;
            if (guard_clfunc) {
                // If we just guarded on the CLFunction, then we have to emit assembly
                // to fetch the values from the defaults array:
                if (i < 3) {
                    r_defaults_array.getAttr(offset, i);
                } else {
                    RewriterVar r_default = r_defaults_array.getAttr(offset, -3);
                    rewrite_args->args.setAttr((i - 3) * sizeof(Box*), r_default, false);
                }
            } else {
                // If we guarded on the BoxedFunction, which has a constant set of defaults,
                // we can embed the default arguments directly into the instructions.
                if (i < 3) {
                    rewrite_args->rewriter->loadConst(i, (intptr_t)default_obj);
                } else {
                    RewriterVar r_default = rewrite_args->rewriter->loadConst(-1, (intptr_t)default_obj);
                    rewrite_args->args.setAttr((i - 3) * sizeof(Box*), r_default, false);
                }
            }
        }

        getArg(i, oarg1, oarg2, oarg3, oargs) = default_obj;
    }



    CompiledFunction* chosen_cf = pickVersion(f, num_output_args, oarg1, oarg2, oarg3, oargs);

    assert(chosen_cf->is_interpreted == (chosen_cf->code == NULL));
    if (chosen_cf->is_interpreted) {
        return interpretFunction(chosen_cf->func, num_output_args, func->closure, oarg1, oarg2, oarg3, oargs);
    } else {
        if (rewrite_args) {
            rewrite_args->rewriter->addDependenceOn(chosen_cf->dependent_callsites);

            RewriterVar var = rewrite_args->rewriter->call((void*)chosen_cf->call);

            rewrite_args->out_rtn = var;
            rewrite_args->out_success = true;
        }

        if (closure)
            return chosen_cf->closure_call(closure, oarg1, oarg2, oarg3, oargs);
        else
            return chosen_cf->call(oarg1, oarg2, oarg3, oargs);
    }
}


Box* runtimeCallInternal(Box* obj, CallRewriteArgs* rewrite_args, ArgPassSpec argspec, Box* arg1, Box* arg2, Box* arg3,
                         Box** args, const std::vector<const std::string*>* keyword_names) {
    int npassed_args = argspec.totalPassed();

    Box* orig_obj = obj;

    if (obj->cls != function_cls && obj->cls != instancemethod_cls) {
        Box* rtn;
        if (rewrite_args) {
            // TODO is this ok?
            // rewrite_args->rewriter->trap();
            rtn = callattrInternal(obj, &_call_str, CLASS_ONLY, rewrite_args, argspec, arg1, arg2, arg3, args,
                                   keyword_names);
        } else {
            rtn = callattrInternal(obj, &_call_str, CLASS_ONLY, NULL, argspec, arg1, arg2, arg3, args, keyword_names);
        }
        if (!rtn)
            raiseExcHelper(TypeError, "'%s' object is not callable", getTypeName(obj)->c_str());
        return rtn;
    }

    if (rewrite_args) {
        if (!rewrite_args->args_guarded) {
            // TODO should know which args don't need to be guarded, ex if we're guaranteed that they
            // already fit, either since the type inferencer could determine that,
            // or because they only need to fit into an UNKNOWN slot.

            if (npassed_args >= 1)
                rewrite_args->arg1.addAttrGuard(BOX_CLS_OFFSET, (intptr_t)arg1->cls);
            if (npassed_args >= 2)
                rewrite_args->arg2.addAttrGuard(BOX_CLS_OFFSET, (intptr_t)arg2->cls);
            if (npassed_args >= 3)
                rewrite_args->arg3.addAttrGuard(BOX_CLS_OFFSET, (intptr_t)arg3->cls);
            for (int i = 3; i < npassed_args; i++) {
                rewrite_args->args.getAttr((i - 3) * sizeof(Box*), -1)
                    .addAttrGuard(BOX_CLS_OFFSET, (intptr_t)args[i - 3]->cls);
            }
            rewrite_args->args_guarded = true;
        }

        rewrite_args->rewriter->addDecision(obj->cls == function_cls ? 1 : 0);
    }

    if (obj->cls == function_cls) {
        BoxedFunction* f = static_cast<BoxedFunction*>(obj);

        // Some functions are sufficiently important that we want them to be able to patchpoint themselves;
        // they can do this by setting the "internal_callable" field:
        CLFunction::InternalCallable callable = f->f->internal_callable;
        if (callable == NULL)
            callable = callFunc;
        return callable(f, rewrite_args, argspec, arg1, arg2, arg3, args, keyword_names);
    } else if (obj->cls == instancemethod_cls) {
        // TODO it's dumb but I should implement patchpoints here as well
        // duplicated with callattr
        BoxedInstanceMethod* im = static_cast<BoxedInstanceMethod*>(obj);

        if (rewrite_args && !rewrite_args->func_guarded) {
            rewrite_args->obj.addAttrGuard(INSTANCEMETHOD_FUNC_OFFSET, (intptr_t)im->func);
        }

        if (npassed_args <= 2) {
            Box* rtn;
            if (rewrite_args) {
                // Kind of weird that we don't need to give this a valid RewriterVar, but it shouldn't need to access it
                // (since we've already guarded on the function).
                CallRewriteArgs srewrite_args(rewrite_args->rewriter, RewriterVar());

                srewrite_args.arg1 = rewrite_args->obj.getAttr(INSTANCEMETHOD_OBJ_OFFSET, 0);
                srewrite_args.func_guarded = true;
                srewrite_args.args_guarded = true;
                if (npassed_args >= 1)
                    srewrite_args.arg2 = rewrite_args->arg1;
                if (npassed_args >= 2)
                    srewrite_args.arg3 = rewrite_args->arg2;

                rtn = runtimeCallInternal(
                    im->func, &srewrite_args,
                    ArgPassSpec(argspec.num_args + 1, argspec.num_keywords, argspec.has_starargs, argspec.has_kwargs),
                    im->obj, arg1, arg2, NULL, keyword_names);

                if (!srewrite_args.out_success) {
                    rewrite_args = NULL;
                } else {
                    rewrite_args->out_rtn = srewrite_args.out_rtn.move(-1);
                }
            } else {
                rtn = runtimeCallInternal(im->func, NULL, ArgPassSpec(argspec.num_args + 1, argspec.num_keywords,
                                                                      argspec.has_starargs, argspec.has_kwargs),
                                          im->obj, arg1, arg2, NULL, keyword_names);
            }
            if (rewrite_args)
                rewrite_args->out_success = true;
            return rtn;
        } else {
            Box** new_args = (Box**)alloca(sizeof(Box*) * (npassed_args + 1 - 3));
            new_args[0] = arg3;
            memcpy(new_args + 1, args, (npassed_args - 3) * sizeof(Box*));
            Box* rtn = runtimeCall(im->func, ArgPassSpec(argspec.num_args + 1, argspec.num_keywords,
                                                         argspec.has_starargs, argspec.has_kwargs),
                                   im->obj, arg1, arg2, new_args, keyword_names);
            return rtn;
        }
    }
    assert(0);
    abort();
}

extern "C" Box* runtimeCall(Box* obj, ArgPassSpec argspec, Box* arg1, Box* arg2, Box* arg3, Box** args,
                            const std::vector<const std::string*>* keyword_names) {
    int npassed_args = argspec.totalPassed();

    static StatCounter slowpath_runtimecall("slowpath_runtimecall");
    slowpath_runtimecall.log();

    int num_orig_args = 2 + std::min(4, npassed_args);
    if (argspec.num_keywords > 0) {
        assert(argspec.num_keywords == keyword_names->size());
        num_orig_args++;
    }
    std::unique_ptr<Rewriter> rewriter(Rewriter::createRewriter(
        __builtin_extract_return_addr(__builtin_return_address(0)), num_orig_args, 2, "runtimeCall"));
    Box* rtn;

    if (rewriter.get()) {
        // rewriter->trap();

        // TODO feel weird about doing this; it either isn't necessary
        // or this kind of thing is necessary in a lot more places
        // rewriter->getArg(1).addGuard(npassed_args);

        CallRewriteArgs rewrite_args(rewriter.get(), rewriter->getArg(0));
        if (npassed_args >= 1)
            rewrite_args.arg1 = rewriter->getArg(2);
        if (npassed_args >= 2)
            rewrite_args.arg2 = rewriter->getArg(3);
        if (npassed_args >= 3)
            rewrite_args.arg3 = rewriter->getArg(4);
        if (npassed_args >= 4)
            rewrite_args.args = rewriter->getArg(5);
        rtn = runtimeCallInternal(obj, &rewrite_args, argspec, arg1, arg2, arg3, args, keyword_names);

        if (!rewrite_args.out_success) {
            rewriter.reset(NULL);
        } else if (rtn) {
            rewrite_args.out_rtn.move(-1);
        }
    } else {
        rtn = runtimeCallInternal(obj, NULL, argspec, arg1, arg2, arg3, args, keyword_names);
    }
    assert(rtn);

    if (rewriter.get()) {
        rewriter->commit();
    }
    return rtn;
}

extern "C" Box* binopInternal(Box* lhs, Box* rhs, int op_type, bool inplace, BinopRewriteArgs* rewrite_args) {
    // TODO handle the case of the rhs being a subclass of the lhs
    // this could get really annoying because you can dynamically make one type a subclass
    // of the other!

    if (rewrite_args) {
        // rewriter->trap();

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

    Box* irtn = NULL;
    if (inplace) {
        std::string iop_name = getInplaceOpName(op_type);
        if (rewrite_args) {
            CallRewriteArgs srewrite_args(rewrite_args->rewriter, rewrite_args->lhs);
            srewrite_args.arg1 = rewrite_args->rhs;
            srewrite_args.args_guarded = true;
            irtn = callattrInternal1(lhs, &iop_name, CLASS_ONLY, &srewrite_args, ArgPassSpec(1), rhs);

            if (!srewrite_args.out_success)
                rewrite_args = NULL;
            else if (irtn)
                rewrite_args->out_rtn = srewrite_args.out_rtn.move(-1);
        } else {
            irtn = callattrInternal1(lhs, &iop_name, CLASS_ONLY, NULL, ArgPassSpec(1), rhs);
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



    const std::string& op_name = getOpName(op_type);
    Box* lrtn;
    if (rewrite_args) {
        CallRewriteArgs srewrite_args(rewrite_args->rewriter, rewrite_args->lhs);
        srewrite_args.arg1 = rewrite_args->rhs;
        lrtn = callattrInternal1(lhs, &op_name, CLASS_ONLY, &srewrite_args, ArgPassSpec(1), rhs);

        if (!srewrite_args.out_success)
            rewrite_args = NULL;
        else if (lrtn)
            rewrite_args->out_rtn = srewrite_args.out_rtn.move(-1);
    } else {
        lrtn = callattrInternal1(lhs, &op_name, CLASS_ONLY, NULL, ArgPassSpec(1), rhs);
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
    if (rewrite_args) {
        assert(rewrite_args->out_success == false);
        rewrite_args = NULL;
    }

    std::string rop_name = getReverseOpName(op_type);
    Box* rrtn = callattrInternal1(rhs, &rop_name, CLASS_ONLY, NULL, ArgPassSpec(1), lhs);
    if (rrtn != NULL && rrtn != NotImplemented)
        return rrtn;


    llvm::StringRef op_sym = getOpSymbol(op_type);
    const char* op_sym_suffix = "";
    if (inplace) {
        op_sym_suffix = "=";
    }

    if (VERBOSITY()) {
        if (inplace) {
            std::string iop_name = getInplaceOpName(op_type);
            if (irtn)
                fprintf(stderr, "%s has %s, but returned NotImplemented\n", getTypeName(lhs)->c_str(),
                        iop_name.c_str());
            else
                fprintf(stderr, "%s does not have %s\n", getTypeName(lhs)->c_str(), iop_name.c_str());
        }

        if (lrtn)
            fprintf(stderr, "%s has %s, but returned NotImplemented\n", getTypeName(lhs)->c_str(), op_name.c_str());
        else
            fprintf(stderr, "%s does not have %s\n", getTypeName(lhs)->c_str(), op_name.c_str());
        if (rrtn)
            fprintf(stderr, "%s has %s, but returned NotImplemented\n", getTypeName(rhs)->c_str(), rop_name.c_str());
        else
            fprintf(stderr, "%s does not have %s\n", getTypeName(rhs)->c_str(), rop_name.c_str());
    }

    raiseExcHelper(TypeError, "unsupported operand type(s) for %s%s: '%s' and '%s'", op_sym.data(), op_sym_suffix,
                   getTypeName(lhs)->c_str(), getTypeName(rhs)->c_str());
}

extern "C" Box* binop(Box* lhs, Box* rhs, int op_type) {
    static StatCounter slowpath_binop("slowpath_binop");
    slowpath_binop.log();
    // static StatCounter nopatch_binop("nopatch_binop");

    // int id = Stats::getStatId("slowpath_binop_" + *getTypeName(lhs) + op_name + *getTypeName(rhs));
    // Stats::log(id);

    std::unique_ptr<Rewriter> rewriter((Rewriter*)NULL);
    // Currently can't patchpoint user-defined binops since we can't assume that just because
    // resolving it one way right now (ex, using the value from lhs.__add__) means that later
    // we'll resolve it the same way, even for the same argument types.
    // TODO implement full resolving semantics inside the rewrite?
    bool can_patchpoint = !isUserDefined(lhs->cls) && !isUserDefined(rhs->cls);
    if (can_patchpoint)
        rewriter.reset(
            Rewriter::createRewriter(__builtin_extract_return_addr(__builtin_return_address(0)), 3, 1, "binop"));

    Box* rtn;
    if (rewriter.get()) {
        // rewriter->trap();
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
    // static StatCounter nopatch_binop("nopatch_binop");

    // int id = Stats::getStatId("slowpath_binop_" + *getTypeName(lhs) + op_name + *getTypeName(rhs));
    // Stats::log(id);

    std::unique_ptr<Rewriter> rewriter((Rewriter*)NULL);
    // Currently can't patchpoint user-defined binops since we can't assume that just because
    // resolving it one way right now (ex, using the value from lhs.__add__) means that later
    // we'll resolve it the same way, even for the same argument types.
    // TODO implement full resolving semantics inside the rewrite?
    bool can_patchpoint = !isUserDefined(lhs->cls) && !isUserDefined(rhs->cls);
    if (can_patchpoint)
        rewriter.reset(
            Rewriter::createRewriter(__builtin_extract_return_addr(__builtin_return_address(0)), 3, 1, "binop"));

    Box* rtn;
    if (rewriter.get()) {
        // rewriter->trap();
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

Box* compareInternal(Box* lhs, Box* rhs, int op_type, CompareRewriteArgs* rewrite_args) {
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

    if (op_type == AST_TYPE::In || op_type == AST_TYPE::NotIn) {
        // TODO do rewrite

        static const std::string str_contains("__contains__");
        Box* contained = callattrInternal1(rhs, &str_contains, CLASS_ONLY, NULL, ArgPassSpec(1), lhs);
        if (contained == NULL) {
            static const std::string str_iter("__iter__");
            Box* iter = callattrInternal0(rhs, &str_iter, CLASS_ONLY, NULL, ArgPassSpec(0));
            if (iter)
                ASSERT(isUserDefined(rhs->cls), "%s should probably have a __contains__", getTypeName(rhs)->c_str());
            RELEASE_ASSERT(iter == NULL, "need to try iterating");

            Box* getitem = typeLookup(rhs->cls, "__getitem__", NULL, NULL);
            if (getitem)
                ASSERT(isUserDefined(rhs->cls), "%s should probably have a __contains__", getTypeName(rhs)->c_str());
            RELEASE_ASSERT(getitem == NULL, "need to try old iteration protocol");

            raiseExcHelper(TypeError, "argument of type '%s' is not iterable", getTypeName(rhs)->c_str());
        }

        bool b = nonzero(contained);
        if (op_type == AST_TYPE::NotIn)
            return boxBool(!b);
        return boxBool(b);
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

    const std::string& op_name = getOpName(op_type);

    Box* lrtn;
    if (rewrite_args) {
        CallRewriteArgs crewrite_args(rewrite_args->rewriter, rewrite_args->lhs);
        crewrite_args.arg1 = rewrite_args->rhs;
        lrtn = callattrInternal1(lhs, &op_name, CLASS_ONLY, &crewrite_args, ArgPassSpec(1), rhs);

        if (!crewrite_args.out_success)
            rewrite_args = NULL;
        else if (lrtn)
            rewrite_args->out_rtn = crewrite_args.out_rtn;
    } else {
        lrtn = callattrInternal1(lhs, &op_name, CLASS_ONLY, NULL, ArgPassSpec(1), rhs);
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


    // TODO patch these cases
    if (rewrite_args) {
        assert(rewrite_args->out_success == false);
        rewrite_args = NULL;
    }

    std::string rop_name = getReverseOpName(op_type);
    Box* rrtn = callattrInternal1(rhs, &rop_name, CLASS_ONLY, NULL, ArgPassSpec(1), lhs);
    if (rrtn != NULL && rrtn != NotImplemented)
        return rrtn;


    if (op_type == AST_TYPE::Eq)
        return boxBool(lhs == rhs);
    if (op_type == AST_TYPE::NotEq)
        return boxBool(lhs != rhs);

    // TODO
    // According to http://docs.python.org/2/library/stdtypes.html#comparisons
    // CPython implementation detail: Objects of different types except numbers are ordered by their type names; objects
    // of the same types that don’t support proper comparison are ordered by their address.

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
    RELEASE_ASSERT(0, "%d", op_type);
}

extern "C" Box* compare(Box* lhs, Box* rhs, int op_type) {
    static StatCounter slowpath_compare("slowpath_compare");
    slowpath_compare.log();
    static StatCounter nopatch_compare("nopatch_compare");

    std::unique_ptr<Rewriter> rewriter(
        Rewriter::createRewriter(__builtin_extract_return_addr(__builtin_return_address(0)), 3, 1, "compare"));

    Box* rtn;
    if (rewriter.get()) {
        // rewriter->trap();
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

    const std::string& op_name = getOpName(op_type);

    Box* attr_func = getclsattr_internal(operand, op_name, NULL, NULL);

    ASSERT(attr_func, "%s.%s", getTypeName(operand)->c_str(), op_name.c_str());

    Box* rtn = runtimeCall0(attr_func, ArgPassSpec(0));
    return rtn;
}

extern "C" Box* getitem(Box* value, Box* slice) {
    // This possibly could just be represented as a single callattr; the only tricky part
    // are the error messages.
    // Ex "(1)[1]" and "(1).__getitem__(1)" give different error messages.

    static StatCounter slowpath_getitem("slowpath_getitem");
    slowpath_getitem.log();
    static std::string str_getitem("__getitem__");

    std::unique_ptr<Rewriter> rewriter(
        Rewriter::createRewriter(__builtin_extract_return_addr(__builtin_return_address(0)), 2, 1, "getitem"));

    Box* rtn;
    if (rewriter.get()) {
        CallRewriteArgs rewrite_args(rewriter.get(), rewriter->getArg(0));
        rewrite_args.arg1 = rewriter->getArg(1);

        rtn = callattrInternal1(value, &str_getitem, CLASS_ONLY, &rewrite_args, ArgPassSpec(1), slice);

        if (!rewrite_args.out_success)
            rewriter.reset(NULL);
        else if (rtn)
            rewrite_args.out_rtn.move(-1);
    } else {
        rtn = callattrInternal1(value, &str_getitem, CLASS_ONLY, NULL, ArgPassSpec(1), slice);
    }

    if (rtn == NULL) {
        // different versions of python give different error messages for this:
        if (PYTHON_VERSION_MAJOR == 2 && PYTHON_VERSION_MINOR < 7) {
            raiseExcHelper(TypeError, "'%s' object is unsubscriptable", getTypeName(value)->c_str()); // 2.6.6
        } else if (PYTHON_VERSION_MAJOR == 2 && PYTHON_VERSION_MINOR == 7 && PYTHON_VERSION_MICRO < 3) {
            raiseExcHelper(TypeError, "'%s' object is not subscriptable", getTypeName(value)->c_str()); // 2.7.1
        } else {
            raiseExcHelper(TypeError, "'%s' object has no attribute '__getitem__'",
                           getTypeName(value)->c_str()); // 2.7.3
        }
    }

    if (rewriter.get())
        rewriter->commit();
    return rtn;
}

// target[slice] = value
extern "C" void setitem(Box* target, Box* slice, Box* value) {
    static StatCounter slowpath_setitem("slowpath_setitem");
    slowpath_setitem.log();
    static std::string str_setitem("__setitem__");

    std::unique_ptr<Rewriter> rewriter(
        Rewriter::createRewriter(__builtin_extract_return_addr(__builtin_return_address(0)), 3, 1, "setitem"));

    Box* rtn;
    RewriterVar r_rtn;
    if (rewriter.get()) {
        CallRewriteArgs rewrite_args(rewriter.get(), rewriter->getArg(0));
        rewrite_args.arg1 = rewriter->getArg(1);
        rewrite_args.arg2 = rewriter->getArg(2);

        rtn = callattrInternal2(target, &str_setitem, CLASS_ONLY, &rewrite_args, ArgPassSpec(2), slice, value);

        if (!rewrite_args.out_success)
            rewriter.reset(NULL);
        else if (rtn)
            r_rtn = rewrite_args.out_rtn;
    } else {
        rtn = callattrInternal2(target, &str_setitem, CLASS_ONLY, NULL, ArgPassSpec(2), slice, value);
    }

    if (rtn == NULL) {
        raiseExcHelper(TypeError, "'%s' object does not support item assignment", getTypeName(target)->c_str());
    }

    if (rewriter.get()) {
        rewriter->commit();
    }
}

// del target[start:end:step]
extern "C" void delitem(Box* target, Box* slice) {
    static StatCounter slowpath_delitem("slowpath_delitem");
    slowpath_delitem.log();
    static std::string str_delitem("__delitem__");

    std::unique_ptr<Rewriter> rewriter(
        Rewriter::createRewriter(__builtin_extract_return_addr(__builtin_return_address(0)), 2, 1, "delitem"));

    Box* rtn;
    RewriterVar r_rtn;
    if (rewriter.get()) {
        CallRewriteArgs rewrite_args(rewriter.get(), rewriter->getArg(0));
        rewrite_args.arg1 = rewriter->getArg(1);

        rtn = callattrInternal1(target, &str_delitem, CLASS_ONLY, &rewrite_args, ArgPassSpec(1), slice);

        if (!rewrite_args.out_success)
            rewriter.reset(NULL);
        else if (rtn)
            r_rtn = rewrite_args.out_rtn;
    } else {
        rtn = callattrInternal1(target, &str_delitem, CLASS_ONLY, NULL, ArgPassSpec(1), slice);
    }

    if (rtn == NULL) {
        raiseExcHelper(TypeError, "'%s' object does not support item deletion", getTypeName(target)->c_str());
    }

    if (rewriter.get()) {
        rewriter->commit();
    }
}

// For use on __init__ return values
static void assertInitNone(Box* obj) {
    if (obj != None) {
        raiseExcHelper(TypeError, "__init__() should return None, not '%s'", getTypeName(obj)->c_str());
    }
}

Box* typeCallInternal(BoxedFunction* f, CallRewriteArgs* rewrite_args, ArgPassSpec argspec, Box* arg1, Box* arg2,
                      Box* arg3, Box** args, const std::vector<const std::string*>* keyword_names) {
    int npassed_args = argspec.totalPassed();

    static StatCounter slowpath_typecall("slowpath_typecall");
    slowpath_typecall.log();

    // if (rewrite_args && VERBOSITY()) {
    // printf("typeCallInternal: %d", rewrite_args->obj.getArgnum());
    // if (npassed_args >= 1) printf(" %d", rewrite_args->arg1.getArgnum());
    // if (npassed_args >= 2) printf(" %d", rewrite_args->arg2.getArgnum());
    // if (npassed_args >= 3) printf(" %d", rewrite_args->arg3.getArgnum());
    // if (npassed_args >= 4) printf(" %d", rewrite_args->args.getArgnum());
    // printf("\n");
    //}


    RewriterVar r_ccls, r_new, r_init;
    Box* new_attr, *init_attr;
    if (rewrite_args) {
        // rewrite_args->rewriter->annotate(0);
        // rewrite_args->rewriter->trap();
        r_ccls = rewrite_args->arg1;
        // This is probably a duplicate, but it's hard to really convince myself of that.
        // Need to create a clear contract of who guards on what
        r_ccls.addGuard((intptr_t)arg1);
    }

    Box* cls = arg1;
    if (cls->cls != type_cls) {
        raiseExcHelper(TypeError, "descriptor '__call__' requires a 'type' object but received an '%s'",
                       getTypeName(cls)->c_str());
    }

    BoxedClass* ccls = static_cast<BoxedClass*>(cls);

    if (rewrite_args) {
        GetattrRewriteArgs grewrite_args(rewrite_args->rewriter, r_ccls);
        grewrite_args.preferred_dest_reg = -2;
        new_attr = typeLookup(ccls, _new_str, &grewrite_args, NULL);

        if (!grewrite_args.out_success)
            rewrite_args = NULL;
        else {
            if (new_attr) {
                r_new = grewrite_args.out_rtn.move(-2);
                r_new.addGuard((intptr_t)new_attr);
            }
        }
    } else {
        new_attr = typeLookup(ccls, _new_str, NULL, NULL);
    }
    assert(new_attr && "This should always resolve");

    if (rewrite_args) {
        GetattrRewriteArgs grewrite_args(rewrite_args->rewriter, r_ccls);
        init_attr = typeLookup(ccls, _init_str, &grewrite_args, NULL);

        if (!grewrite_args.out_success)
            rewrite_args = NULL;
        else {
            if (init_attr) {
                r_init = grewrite_args.out_rtn;
                r_init.addGuard((intptr_t)init_attr);
            }
        }
    } else {
        init_attr = typeLookup(ccls, _init_str, NULL, NULL);
    }
    // The init_attr should always resolve as well, but doesn't yet

    Box* made;
    RewriterVar r_made;

    auto new_argspec = argspec;
    if (npassed_args > 1 && new_attr == typeLookup(object_cls, _new_str, NULL, NULL)) {
        if (init_attr == typeLookup(object_cls, _init_str, NULL, NULL)) {
            raiseExcHelper(TypeError, "object.__new__() takes no parameters");
        } else {
            new_argspec = ArgPassSpec(1);
        }
    }

    if (rewrite_args) {
        if (init_attr)
            r_init.push();
        if (npassed_args >= 1)
            r_ccls.push();
        if (npassed_args >= 2)
            rewrite_args->arg2.push();
        if (npassed_args >= 3)
            rewrite_args->arg3.push();
        if (npassed_args >= 4)
            rewrite_args->args.push();

        CallRewriteArgs srewrite_args(rewrite_args->rewriter, r_new);
        if (npassed_args >= 1)
            srewrite_args.arg1 = r_ccls;
        if (npassed_args >= 2)
            srewrite_args.arg2 = rewrite_args->arg2;
        if (npassed_args >= 3)
            srewrite_args.arg3 = rewrite_args->arg3;
        if (npassed_args >= 4)
            srewrite_args.args = rewrite_args->args;
        srewrite_args.args_guarded = true;
        srewrite_args.func_guarded = true;

        r_new.push();

        made = runtimeCallInternal(new_attr, &srewrite_args, new_argspec, cls, arg2, arg3, args, keyword_names);

        if (!srewrite_args.out_success)
            rewrite_args = NULL;
        else {
            r_made = srewrite_args.out_rtn;

            r_new = rewrite_args->rewriter->pop(0);
            r_made = r_made.move(-1);

            if (npassed_args >= 4)
                rewrite_args->args = rewrite_args->rewriter->pop(3);
            if (npassed_args >= 3)
                rewrite_args->arg3 = rewrite_args->rewriter->pop(2);
            if (npassed_args >= 2)
                rewrite_args->arg2 = rewrite_args->rewriter->pop(1);
            if (npassed_args >= 1)
                r_ccls = rewrite_args->arg1 = rewrite_args->rewriter->pop(0);
            if (init_attr)
                r_init = rewrite_args->rewriter->pop(-2);
        }
    } else {
        made = runtimeCallInternal(new_attr, NULL, new_argspec, cls, arg2, arg3, args, keyword_names);
    }

    assert(made);
    // If this is true, not supposed to call __init__:
    RELEASE_ASSERT(made->cls == ccls, "allowed but unsupported");

    if (init_attr) {
        Box* initrtn;
        if (rewrite_args) {
            CallRewriteArgs srewrite_args(rewrite_args->rewriter, r_init);
            if (npassed_args >= 1)
                srewrite_args.arg1 = r_made;
            if (npassed_args >= 2)
                srewrite_args.arg2 = rewrite_args->arg2;
            if (npassed_args >= 3)
                srewrite_args.arg3 = rewrite_args->arg3;
            if (npassed_args >= 4)
                srewrite_args.args = rewrite_args->args;
            srewrite_args.args_guarded = true;
            srewrite_args.func_guarded = true;

            r_made.push();
            r_init.push();
            // initrtn = callattrInternal(ccls, &_init_str, INST_ONLY, &srewrite_args, argspec, made, arg2, arg3, args,
            // keyword_names);
            initrtn = runtimeCallInternal(init_attr, &srewrite_args, argspec, made, arg2, arg3, args, keyword_names);

            if (!srewrite_args.out_success)
                rewrite_args = NULL;
            else {
                srewrite_args.out_rtn.move(0);
                rewrite_args->rewriter->call((void*)assertInitNone);

                r_init = rewrite_args->rewriter->pop(0);
                r_made = rewrite_args->rewriter->pop(-1);
            }
        } else {
            // initrtn = callattrInternal(ccls, &_init_str, INST_ONLY, NULL, argspec, made, arg2, arg3, args,
            // keyword_names);
            initrtn = runtimeCallInternal(init_attr, NULL, argspec, made, arg2, arg3, args, keyword_names);
        }
        assertInitNone(initrtn);
    } else {
        // TODO this shouldn't be reached
        // assert(0 && "I don't think this should be reached");
        if (new_attr == NULL && npassed_args != 1) {
            // TODO not npassed args, since the starargs or kwargs could be null
            raiseExcHelper(TypeError, "object.__new__() takes no parameters");
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
        return typeCallInternal1(NULL, NULL, ArgPassSpec(1), obj);
    else if (vararg->size == 1)
        return typeCallInternal2(NULL, NULL, ArgPassSpec(2), obj, vararg->elts->elts[0]);
    else if (vararg->size == 2)
        return typeCallInternal3(NULL, NULL, ArgPassSpec(3), obj, vararg->elts->elts[0], vararg->elts->elts[1]);
    else
        return typeCallInternal(NULL, NULL, ArgPassSpec(1 + vararg->size), obj, vararg->elts->elts[0],
                                vararg->elts->elts[1], &vararg->elts->elts[2], NULL);
}

Box* typeNew(Box* cls, Box* obj) {
    assert(cls == type_cls);

    BoxedClass* rtn = obj->cls;
    return rtn;
}

extern "C" Box* getGlobal(BoxedModule* m, std::string* name) {
    static StatCounter slowpath_getglobal("slowpath_getglobal");
    slowpath_getglobal.log();
    static StatCounter nopatch_getglobal("nopatch_getglobal");

    if (VERBOSITY() >= 2) {
#if !DISABLE_STATS
        std::string per_name_stat_name = "getglobal__" + *name;
        int id = Stats::getStatId(per_name_stat_name);
        Stats::log(id);
#endif
    }

    { /* anonymous scope to make sure destructors get run before we err out */
        std::unique_ptr<Rewriter> rewriter(
            Rewriter::createRewriter(__builtin_extract_return_addr(__builtin_return_address(0)), 3, 1, "getGlobal"));

        Box* r;
        if (rewriter.get()) {
            // rewriter->trap();

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

    raiseExcHelper(NameError, "global name '%s' is not defined", name->c_str());
}

// TODO I feel like importing should go somewhere else; it's more closely tied to codegen
// than to the object model.
extern "C" Box* import(const std::string* name) {
    assert(name);

    static StatCounter slowpath_import("slowpath_import");
    slowpath_import.log();

    BoxedDict* sys_modules = getSysModulesDict();
    Box* s = boxStringPtr(name);
    if (sys_modules->d.find(s) != sys_modules->d.end())
        return sys_modules->d[s];

    BoxedList* sys_path = getSysPath();
    if (sys_path->cls != list_cls) {
        raiseExcHelper(RuntimeError, "sys.path must be a list of directory name");
    }

    llvm::SmallString<128> joined_path;
    for (int i = 0; i < sys_path->size; i++) {
        Box* _p = sys_path->elts->elts[i];
        if (_p->cls != str_cls)
            continue;
        BoxedString* p = static_cast<BoxedString*>(_p);

        joined_path.clear();
        llvm::sys::path::append(joined_path, p->s, *name + ".py");
        std::string fn(joined_path.str());

        if (VERBOSITY() >= 2)
            printf("Searching for %s at %s...\n", name->c_str(), fn.c_str());

        bool exists;
        llvm::error_code code = llvm::sys::fs::exists(joined_path.str(), exists);
#if LLVMREV < 210072
        assert(code == 0);
#else
        assert(!code);
#endif
        if (!exists)
            continue;

        if (VERBOSITY() >= 1)
            printf("Importing %s from %s\n", name->c_str(), fn.c_str());

        // TODO duplication with jit.cpp:
        BoxedModule* module = createModule(*name, fn);
        AST_Module* ast = caching_parse(fn.c_str());
        compileAndRunModule(ast, module);
        return module;
    }

    if (*name == "test") {
        return getTestModule();
    }

    raiseExcHelper(ImportError, "No module named %s", name->c_str());
}

extern "C" Box* importFrom(Box* _m, const std::string* name) {
    assert(_m->cls == module_cls);

    BoxedModule* m = static_cast<BoxedModule*>(_m);

    Box* r = m->getattr(*name, NULL, NULL);
    if (r)
        return r;

    raiseExcHelper(ImportError, "cannot import name %s", name->c_str());
}
}
