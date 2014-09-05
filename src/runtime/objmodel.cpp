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

#include "asm_writing/icinfo.h"
#include "asm_writing/rewriter.h"
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
#include "runtime/classobj.h"
#include "runtime/float.h"
#include "runtime/generator.h"
#include "runtime/iterobject.h"
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
    RewriterVarUsage obj;
    Location destination;
    bool call_done_guarding;

    bool out_success;
    RewriterVarUsage out_rtn;

    bool obj_hcls_guarded;

    GetattrRewriteArgs(Rewriter* rewriter, RewriterVarUsage&& obj, Location destination, bool call_done_guarding)
        : rewriter(rewriter), obj(std::move(obj)), destination(destination), call_done_guarding(call_done_guarding),
          out_success(false), out_rtn(RewriterVarUsage::empty()), obj_hcls_guarded(false) {}

    ~GetattrRewriteArgs() {
        if (!out_success) {
            ensureAllDone();
        }
    }

    void ensureAllDone() { obj.ensureDoneUsing(); }
};

struct SetattrRewriteArgs {
    Rewriter* rewriter;
    RewriterVarUsage obj, attrval;
    bool call_done_guarding;

    bool out_success;

    SetattrRewriteArgs(Rewriter* rewriter, RewriterVarUsage&& obj, RewriterVarUsage&& attrval, bool call_done_guarding)
        : rewriter(rewriter), obj(std::move(obj)), attrval(std::move(attrval)), call_done_guarding(call_done_guarding),
          out_success(false) {}

    ~SetattrRewriteArgs() {
        if (!out_success) {
            ensureAllDone();
        }
    }

    void ensureAllDone() { obj.ensureDoneUsing(); }
};

struct DelattrRewriteArgs {
    Rewriter* rewriter;
    RewriterVarUsage obj;
    bool call_done_guarding;

    bool out_success;

    DelattrRewriteArgs(Rewriter* rewriter, RewriterVarUsage&& obj, bool call_done_guarding)
        : rewriter(rewriter), obj(std::move(obj)), call_done_guarding(call_done_guarding), out_success(false) {}

    ~DelattrRewriteArgs() {
        if (!out_success) {
            ensureAllDone();
        }
    }

    void ensureAllDone() { obj.ensureDoneUsing(); }
};

struct LenRewriteArgs {
    Rewriter* rewriter;
    RewriterVarUsage obj;
    Location destination;
    bool call_done_guarding;

    bool out_success;
    RewriterVarUsage out_rtn;

    LenRewriteArgs(Rewriter* rewriter, RewriterVarUsage&& obj, Location destination, bool call_done_guarding)
        : rewriter(rewriter), obj(std::move(obj)), destination(destination), call_done_guarding(call_done_guarding),
          out_success(false), out_rtn(RewriterVarUsage::empty()) {}

    ~LenRewriteArgs() {
        if (!out_success) {
            ensureAllDone();
        }
    }

    void ensureAllDone() { obj.ensureDoneUsing(); }
};

struct CallRewriteArgs {
    Rewriter* rewriter;
    RewriterVarUsage obj;
    RewriterVarUsage arg1, arg2, arg3, args;
    bool func_guarded;
    bool args_guarded;
    Location destination;
    bool call_done_guarding;

    bool out_success;
    RewriterVarUsage out_rtn;

    CallRewriteArgs(Rewriter* rewriter, RewriterVarUsage&& obj, Location destination, bool call_done_guarding)
        : rewriter(rewriter), obj(std::move(obj)), arg1(RewriterVarUsage::empty()), arg2(RewriterVarUsage::empty()),
          arg3(RewriterVarUsage::empty()), args(RewriterVarUsage::empty()), func_guarded(false), args_guarded(false),
          destination(destination), call_done_guarding(call_done_guarding), out_success(false),
          out_rtn(RewriterVarUsage::empty()) {}

    ~CallRewriteArgs() {
        if (!out_success) {
            ensureAllDone();
        }
    }

    void ensureAllDone() {
        obj.ensureDoneUsing();
        arg1.ensureDoneUsing();
        arg2.ensureDoneUsing();
        arg3.ensureDoneUsing();
        args.ensureDoneUsing();
    }
};

struct BinopRewriteArgs {
    Rewriter* rewriter;
    RewriterVarUsage lhs, rhs;
    Location destination;
    bool call_done_guarding;

    bool out_success;
    RewriterVarUsage out_rtn;

    BinopRewriteArgs(Rewriter* rewriter, RewriterVarUsage&& lhs, RewriterVarUsage&& rhs, Location destination,
                     bool call_done_guarding)
        : rewriter(rewriter), lhs(std::move(lhs)), rhs(std::move(rhs)), destination(destination),
          call_done_guarding(call_done_guarding), out_success(false), out_rtn(RewriterVarUsage::empty()) {}

    ~BinopRewriteArgs() {
        if (!out_success) {
            ensureAllDone();
        }
    }

    void ensureAllDone() {
        lhs.ensureDoneUsing();
        rhs.ensureDoneUsing();
    }
};

struct CompareRewriteArgs {
    Rewriter* rewriter;
    RewriterVarUsage lhs, rhs;
    Location destination;
    bool call_done_guarding;

    bool out_success;
    RewriterVarUsage out_rtn;

    CompareRewriteArgs(Rewriter* rewriter, RewriterVarUsage&& lhs, RewriterVarUsage&& rhs, Location destination,
                       bool call_done_guarding)
        : rewriter(rewriter), lhs(std::move(lhs)), rhs(std::move(rhs)), destination(destination),
          call_done_guarding(call_done_guarding), out_success(false), out_rtn(RewriterVarUsage::empty()) {}

    ~CompareRewriteArgs() {
        if (!out_success) {
            ensureAllDone();
        }
    }

    void ensureAllDone() {
        lhs.ensureDoneUsing();
        rhs.ensureDoneUsing();
    }
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

extern "C" bool softspace(Box* b, bool newval) {
    assert(b);

    if (isSubclass(b->cls, file_cls)) {
        bool& ss = static_cast<BoxedFile*>(b)->softspace;
        bool r = ss;
        ss = newval;
        return r;
    }

    bool r;
    Box* gotten = b->getattr("softspace");
    if (!gotten) {
        r = 0;
    } else {
        r = nonzero(gotten);
    }
    b->setattr("softspace", boxInt(newval), NULL);
    return r;
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

extern "C" void assertNameDefined(bool b, const char* name, BoxedClass* exc_cls, bool local_var_msg) {
    if (!b) {
        if (local_var_msg)
            raiseExcHelper(exc_cls, "local variable '%s' referenced before assignment", name);
        else
            raiseExcHelper(exc_cls, "name '%s' is not defined", name);
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

static void _checkUnpackingLength(i64 expected, i64 given) {
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

extern "C" Box** unpackIntoArray(Box* obj, int64_t expected_size) {
    assert(expected_size > 0);

    if (obj->cls == tuple_cls) {
        BoxedTuple* t = static_cast<BoxedTuple*>(obj);
        _checkUnpackingLength(expected_size, t->elts.size());
        return &t->elts[0];
    }

    if (obj->cls == list_cls) {
        BoxedList* l = static_cast<BoxedList*>(obj);
        _checkUnpackingLength(expected_size, l->size);
        return &l->elts->elts[0];
    }

    BoxedTuple::GCVector elts;
    for (auto e : obj->pyElements()) {
        elts.push_back(e);
        if (elts.size() > expected_size)
            break;
    }

    _checkUnpackingLength(expected_size, elts.size());
    return &elts[0];
}

BoxedClass::BoxedClass(BoxedClass* metaclass, BoxedClass* base, gcvisit_func gc_visit, int attrs_offset,
                       int instance_size, bool is_user_defined)
    : BoxVar(metaclass, 0), tp_basicsize(instance_size), tp_dealloc(NULL), base(base), gc_visit(gc_visit),
      attrs_offset(attrs_offset), is_constant(false), is_user_defined(is_user_defined) {
    if (metaclass == NULL) {
        assert(type_cls == NULL);
    } else {
        assert(isSubclass(metaclass, type_cls));
    }

    assert(tp_dealloc == NULL);

    if (gc_visit == NULL) {
        assert(base);
        this->gc_visit = base->gc_visit;
    }
    assert(this->gc_visit);

    if (!base) {
        assert(object_cls == nullptr);
        // we're constructing 'object'
        // Will have to add __base__ = None later
    } else {
        assert(object_cls);
        if (base->attrs_offset)
            RELEASE_ASSERT(attrs_offset == base->attrs_offset, "");
        assert(tp_basicsize >= base->tp_basicsize);
    }

    if (base && cls && str_cls)
        giveAttr("__base__", base);

    assert(tp_basicsize % sizeof(void*) == 0); // Not critical I suppose, but probably signals a bug
    if (attrs_offset) {
        assert(tp_basicsize >= attrs_offset + sizeof(HCAttrs));
        assert(attrs_offset % sizeof(void*) == 0); // Not critical I suppose, but probably signals a bug
    }

    if (!is_user_defined)
        gc::registerPermanentRoot(this);
}

std::string getFullNameOfClass(BoxedClass* cls) {
    Box* b = cls->getattr("__name__");
    assert(b);
    ASSERT(b->cls == str_cls, "%p", b->cls);
    BoxedString* name = static_cast<BoxedString*>(b);

    b = cls->getattr("__module__");
    if (!b)
        return name->s;
    assert(b);
    if (b->cls != str_cls)
        return name->s;

    BoxedString* module = static_cast<BoxedString*>(b);

    return module->s + "." + name->s;
}

std::string getFullTypeName(Box* o) {
    return getFullNameOfClass(o->cls);
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

/**
 * del attr from current HiddenClass, pertain the orders of remaining attrs
 */
HiddenClass* HiddenClass::delAttrToMakeHC(const std::string& attr) {
    int idx = getOffset(attr);
    assert(idx >= 0);

    std::vector<std::string> new_attrs(attr_offsets.size() - 1);
    for (auto it = attr_offsets.begin(); it != attr_offsets.end(); ++it) {
        if (it->second < idx)
            new_attrs[it->second] = it->first;
        else if (it->second > idx) {
            new_attrs[it->second - 1] = it->first;
        }
    }

    // TODO we can first locate the parent HiddenClass of the deleted
    // attribute and hence avoid creation of its ancestors.
    HiddenClass* cur = root_hcls;
    for (const auto& attr : new_attrs) {
        cur = cur->getOrMakeChild(attr);
    }
    return cur;
}

Box::Box(BoxedClass* cls) : cls(cls) {
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

Box* Box::getattr(const std::string& attr, GetattrRewriteArgs* rewrite_args) {
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
        rewrite_args->obj.addAttrGuard(BOX_CLS_OFFSET, (intptr_t)cls);
        rewrite_args->out_success = true;
    }

    if (!cls->instancesHaveAttrs()) {
        if (rewrite_args) {
            rewrite_args->obj.setDoneUsing();
            if (rewrite_args->call_done_guarding)
                rewrite_args->rewriter->setDoneGuarding();
        }

        return NULL;
    }

    HCAttrs* attrs = getAttrsPtr();
    HiddenClass* hcls = attrs->hcls;

    if (rewrite_args) {
        if (!rewrite_args->obj_hcls_guarded)
            rewrite_args->obj.addAttrGuard(cls->attrs_offset + HCATTRS_HCLS_OFFSET, (intptr_t)hcls);

        if (rewrite_args->call_done_guarding)
            rewrite_args->rewriter->setDoneGuarding();
    }

    int offset = hcls->getOffset(attr);
    if (offset == -1) {
        if (rewrite_args) {
            rewrite_args->obj.setDoneUsing();
        }
        return NULL;
    }

    if (rewrite_args) {
        // TODO using the output register as the temporary makes register allocation easier
        // since we don't need to clobber a register, but does it make the code slower?
        RewriterVarUsage attrs = rewrite_args->obj.getAttr(cls->attrs_offset + HCATTRS_ATTRS_OFFSET,
                                                           RewriterVarUsage::Kill, Location::any());
        rewrite_args->out_rtn
            = attrs.getAttr(offset * sizeof(Box*) + ATTRLIST_ATTRS_OFFSET, RewriterVarUsage::Kill, Location::any());
    }

    Box* rtn = attrs->attr_list->attrs[offset];
    return rtn;
}

// TODO should centralize all of these:
static const std::string _call_str("__call__"), _new_str("__new__"), _init_str("__init__");

void Box::setattr(const std::string& attr, Box* val, SetattrRewriteArgs* rewrite_args) {
    assert(cls->instancesHaveAttrs());
    assert(gc::isValidGCObject(val));

    // Have to guard on the memory layout of this object.
    // Right now, guard on the specific Python-class, which in turn
    // specifies the C structure.
    // In the future, we could create another field (the flavor?)
    // that also specifies the structure and can include multiple
    // classes.
    // Only matters if we end up getting multiple classes with the same
    // structure (ex user class) and the same hidden classes, because
    // otherwise the guard will fail anyway.;
    if (rewrite_args)
        rewrite_args->obj.addAttrGuard(BOX_CLS_OFFSET, (intptr_t)cls);


    static const std::string none_str("None");
    static const std::string getattr_str("__getattr__");
    static const std::string getattribute_str("__getattribute__");

    RELEASE_ASSERT(attr != none_str || this == builtins_module, "can't assign to None");

    if (isSubclass(this->cls, type_cls)) {
        BoxedClass* self = static_cast<BoxedClass*>(this);

        if (attr == getattr_str || attr == getattribute_str) {
            // Will have to embed the clear in the IC, so just disable the patching for now:
            rewrite_args = NULL;

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

    if (rewrite_args) {
        rewrite_args->obj.addAttrGuard(cls->attrs_offset + HCATTRS_HCLS_OFFSET, (intptr_t)hcls);

        if (rewrite_args->call_done_guarding)
            rewrite_args->rewriter->setDoneGuarding();
        // rewrite_args->rewriter->addDecision(offset == -1 ? 1 : 0);
    }

    if (offset >= 0) {
        assert(offset < numattrs);
        Box* prev = attrs->attr_list->attrs[offset];
        attrs->attr_list->attrs[offset] = val;

        if (rewrite_args) {

            RewriterVarUsage r_hattrs = rewrite_args->obj.getAttr(cls->attrs_offset + HCATTRS_ATTRS_OFFSET,
                                                                  RewriterVarUsage::Kill, Location::any());

            r_hattrs.setAttr(offset * sizeof(Box*) + ATTRLIST_ATTRS_OFFSET, std::move(rewrite_args->attrval));
            r_hattrs.setDoneUsing();

            rewrite_args->out_success = true;
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

    RewriterVarUsage r_new_array2(RewriterVarUsage::empty());
    int new_size = sizeof(HCAttrs::AttrList) + sizeof(Box*) * (numattrs + 1);
    if (numattrs == 0) {
        attrs->attr_list = (HCAttrs::AttrList*)gc_alloc(new_size, gc::GCKind::UNTRACKED);
        if (rewrite_args) {
            RewriterVarUsage r_newsize = rewrite_args->rewriter->loadConst(new_size, Location::forArg(0));
            RewriterVarUsage r_kind
                = rewrite_args->rewriter->loadConst((int)gc::GCKind::UNTRACKED, Location::forArg(1));
            r_new_array2
                = rewrite_args->rewriter->call(false, (void*)gc::gc_alloc, std::move(r_newsize), std::move(r_kind));
        }
    } else {
        attrs->attr_list = (HCAttrs::AttrList*)gc::gc_realloc(attrs->attr_list, new_size);
        if (rewrite_args) {
            RewriterVarUsage r_oldarray = rewrite_args->obj.getAttr(cls->attrs_offset + HCATTRS_ATTRS_OFFSET,
                                                                    RewriterVarUsage::NoKill, Location::forArg(0));
            RewriterVarUsage r_newsize = rewrite_args->rewriter->loadConst(new_size, Location::forArg(1));
            r_new_array2 = rewrite_args->rewriter->call(false, (void*)gc::gc_realloc, std::move(r_oldarray),
                                                        std::move(r_newsize));
        }
    }
    // Don't set the new hcls until after we do the allocation for the new attr_list;
    // that allocation can cause a collection, and we want the collector to always
    // see a consistent state between the hcls and the attr_list
    attrs->hcls = new_hcls;

    if (rewrite_args) {
        r_new_array2.setAttr(numattrs * sizeof(Box*) + ATTRLIST_ATTRS_OFFSET, std::move(rewrite_args->attrval));
        rewrite_args->obj.setAttr(cls->attrs_offset + HCATTRS_ATTRS_OFFSET, std::move(r_new_array2));

        RewriterVarUsage r_hcls = rewrite_args->rewriter->loadConst((intptr_t)new_hcls);
        rewrite_args->obj.setAttr(cls->attrs_offset + HCATTRS_HCLS_OFFSET, std::move(r_hcls));
        rewrite_args->obj.setDoneUsing();

        rewrite_args->out_success = true;
    }
    attrs->attr_list->attrs[numattrs] = val;
}

Box* typeLookup(BoxedClass* cls, const std::string& attr, GetattrRewriteArgs* rewrite_args) {
    Box* val;

    if (rewrite_args) {
        assert(!rewrite_args->out_success);

        RewriterVarUsage obj_saved = rewrite_args->obj.addUse();
        bool call_done_guarding = rewrite_args->call_done_guarding;
        rewrite_args->call_done_guarding = false;

        val = cls->getattr(attr, rewrite_args);
        assert(rewrite_args->out_success);
        if (!val and cls->base) {
            rewrite_args->out_success = false;
            rewrite_args->obj = obj_saved.getAttr(offsetof(BoxedClass, base), RewriterVarUsage::Kill);
            val = typeLookup(cls->base, attr, rewrite_args);
        } else {
            obj_saved.setDoneUsing();
        }
        if (call_done_guarding)
            rewrite_args->rewriter->setDoneGuarding();
        return val;
    } else {
        val = cls->getattr(attr, NULL);
        if (!val and cls->base)
            return typeLookup(cls->base, attr, NULL);
        return val;
    }
}

bool isNondataDescriptorInstanceSpecialCase(Box* descr) {
    return descr->cls == function_cls || descr->cls == method_cls;
}

Box* nondataDescriptorInstanceSpecialCases(GetattrRewriteArgs* rewrite_args, Box* obj, Box* descr,
                                           RewriterVarUsage& r_descr, bool for_call, bool* should_bind_out) {
    // Special case: non-data descriptor: function
    if (descr->cls == function_cls || descr->cls == method_cls) {
        if (!for_call) {
            if (rewrite_args) {
                // can't guard after because we make this call... the call is trivial enough
                // that we can probably work around it if it's important, but otherwise, if
                // this triggers, just abort rewriting, I guess
                assert(rewrite_args->call_done_guarding);
                rewrite_args->rewriter->setDoneGuarding();
                rewrite_args->out_rtn = rewrite_args->rewriter->call(false, (void*)boxInstanceMethod,
                                                                     std::move(rewrite_args->obj), std::move(r_descr));
                rewrite_args->out_success = true;
            }
            return boxInstanceMethod(obj, descr);
        } else {
            if (rewrite_args) {
                if (rewrite_args->call_done_guarding)
                    rewrite_args->rewriter->setDoneGuarding();
                rewrite_args->obj.setDoneUsing();
                rewrite_args->out_rtn = std::move(r_descr);
                rewrite_args->out_success = true;
            }
            *should_bind_out = true;
            return descr;
        }
    }

    return NULL;
}

Box* descriptorClsSpecialCases(GetattrRewriteArgs* rewrite_args, BoxedClass* cls, Box* descr, RewriterVarUsage& r_descr,
                               bool for_call, bool* should_bind_out) {
    // Special case: functions
    if (descr->cls == function_cls || descr->cls == method_cls) {
        if (rewrite_args)
            r_descr.addAttrGuard(BOX_CLS_OFFSET, (uint64_t)descr->cls);

        if (!for_call) {
            if (rewrite_args) {
                assert(rewrite_args->call_done_guarding);
                rewrite_args->rewriter->setDoneGuarding();
                rewrite_args->obj.setDoneUsing();

                // return an unbound instancemethod
                rewrite_args->out_rtn
                    = rewrite_args->rewriter->call(false, (void*)boxUnboundInstanceMethod, std::move(r_descr));
                rewrite_args->out_success = true;
            }
            return boxUnboundInstanceMethod(descr);
        } else {
            if (rewrite_args) {
                if (rewrite_args->call_done_guarding)
                    rewrite_args->rewriter->setDoneGuarding();
                rewrite_args->obj.setDoneUsing();
                rewrite_args->out_success = true;
                rewrite_args->out_rtn = std::move(r_descr);
            }
            // leave should_bind_out set to false
            return descr;
        }
    }

    // Special case: member descriptor
    if (descr->cls == member_cls) {
        if (rewrite_args)
            r_descr.addAttrGuard(BOX_CLS_OFFSET, (uint64_t)descr->cls);

        if (rewrite_args) {
            assert(rewrite_args->call_done_guarding);
            rewrite_args->rewriter->setDoneGuarding();
            rewrite_args->obj.setDoneUsing();

            // Actually just return val (it's a descriptor but only
            // has special behaviour for instance lookups - see below)
            rewrite_args->out_rtn = std::move(r_descr);
            rewrite_args->out_success = true;
        }
        return descr;
    }

    return NULL;
}

Box* dataDescriptorInstanceSpecialCases(GetattrRewriteArgs* rewrite_args, Box* obj, Box* descr,
                                        RewriterVarUsage& r_descr, bool for_call, bool* should_bind_out) {
    // Special case: data descriptor: member descriptor
    if (descr->cls == member_cls) {
        BoxedMemberDescriptor* member_desc = static_cast<BoxedMemberDescriptor*>(descr);
        // TODO should also have logic to raise a type error if type of obj is wrong

        if (rewrite_args) {
            if (rewrite_args->call_done_guarding)
                rewrite_args->rewriter->setDoneGuarding();
            r_descr.setDoneUsing();
        }

        switch (member_desc->type) {
            case BoxedMemberDescriptor::OBJECT: {
                assert(member_desc->offset % sizeof(Box*) == 0);
                Box* rtn = reinterpret_cast<Box**>(obj)[member_desc->offset / sizeof(Box*)];
                RELEASE_ASSERT(rtn, "");

                if (rewrite_args) {
                    rewrite_args->out_rtn = rewrite_args->obj.getAttr(
                        member_desc->offset, RewriterVarUsage::KillFlag::Kill, rewrite_args->destination);
                    rewrite_args->out_success = true;
                }

                return rtn;
            }
            case BoxedMemberDescriptor::BYTE: {
                // TODO rewriter stuff for these other cases as well
                rewrite_args = NULL;
                int8_t rtn = reinterpret_cast<int8_t*>(obj)[member_desc->offset];
                return boxInt(rtn);
            }
            case BoxedMemberDescriptor::BOOL: {
                rewrite_args = NULL;
                bool rtn = reinterpret_cast<bool*>(obj)[member_desc->offset];
                return boxBool(rtn);
            }
            case BoxedMemberDescriptor::INT: {
                rewrite_args = NULL;
                int rtn = reinterpret_cast<int*>(obj)[member_desc->offset / sizeof(int)];
                return boxInt(rtn);
            }
            case BoxedMemberDescriptor::FLOAT: {
                rewrite_args = NULL;
                double rtn = reinterpret_cast<double*>(obj)[member_desc->offset / sizeof(double)];
                return boxFloat(rtn);
            }
            default:
                RELEASE_ASSERT(0, "%d", member_desc->type);
        }
    }

    return NULL;
}

inline Box* getclsattr_internal(Box* obj, const std::string& attr, GetattrRewriteArgs* rewrite_args) {
    return getattrInternalGeneral(obj, attr, rewrite_args,
                                  /* cls_only */ true,
                                  /* for_call */ false, NULL);
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
    std::unique_ptr<Rewriter> rewriter(
        Rewriter::createRewriter(__builtin_extract_return_addr(__builtin_return_address(0)), 2, "getclsattr"));

    if (rewriter.get()) {
        // rewriter->trap();
        GetattrRewriteArgs rewrite_args(rewriter.get(), rewriter->getArg(0), rewriter->getReturnDestination(), true);
        gotten = getclsattr_internal(obj, attr, &rewrite_args);

        if (rewrite_args.out_success && gotten) {
            rewriter->commitReturning(std::move(rewrite_args.out_rtn));
        }
#endif
}
else {
    gotten = getclsattr_internal(obj, attr, NULL);
}
RELEASE_ASSERT(gotten, "%s:%s", getTypeName(obj)->c_str(), attr);

return gotten;
}


// Does a simple call of the descriptor's __get__ if it exists;
// this function is useful for custom getattribute implementations that already know whether the descriptor
// came from the class or not.
Box* processDescriptor(Box* obj, Box* inst, Box* owner) {
    static const std::string get_str("__get__");
    Box* descr_r
        = callattrInternal(obj, &get_str, LookupScope::CLASS_ONLY, NULL, ArgPassSpec(2), inst, owner, NULL, NULL, NULL);
    if (descr_r)
        return descr_r;
    return obj;
}


static Box* (*runtimeCall0)(Box*, ArgPassSpec) = (Box * (*)(Box*, ArgPassSpec))runtimeCall;
static Box* (*runtimeCall1)(Box*, ArgPassSpec, Box*) = (Box * (*)(Box*, ArgPassSpec, Box*))runtimeCall;
static Box* (*runtimeCall2)(Box*, ArgPassSpec, Box*, Box*) = (Box * (*)(Box*, ArgPassSpec, Box*, Box*))runtimeCall;
static Box* (*runtimeCall3)(Box*, ArgPassSpec, Box*, Box*, Box*)
    = (Box * (*)(Box*, ArgPassSpec, Box*, Box*, Box*))runtimeCall;

Box* getattrInternalGeneral(Box* obj, const std::string& attr, GetattrRewriteArgs* rewrite_args, bool cls_only,
                            bool for_call, bool* should_bind_out) {
    if (for_call) {
        *should_bind_out = false;
    }

    if (obj->cls == closure_cls) {
        Box* val = NULL;
        if (rewrite_args) {
            GetattrRewriteArgs hrewrite_args(rewrite_args->rewriter, rewrite_args->obj.addUse(),
                                             rewrite_args->destination, false);
            val = obj->getattr(attr, &hrewrite_args);

            if (!hrewrite_args.out_success) {
                rewrite_args = NULL;
            } else if (val) {
                if (rewrite_args->call_done_guarding)
                    rewrite_args->rewriter->setDoneGuarding();
                rewrite_args->out_rtn = std::move(hrewrite_args.out_rtn);
                rewrite_args->out_success = true;
                rewrite_args->obj.setDoneUsing();
                return val;
            }
        } else {
            val = obj->getattr(attr, NULL);
            if (val) {
                return val;
            }
        }

        // If val doesn't exist, then we move up to the parent closure
        // TODO closures should get their own treatment, but now just piggy-back on the
        // normal hidden-class IC logic.
        // Can do better since we don't need to guard on the cls (always going to be closure)
        BoxedClosure* closure = static_cast<BoxedClosure*>(obj);
        if (closure->parent) {
            if (rewrite_args) {
                rewrite_args->obj = rewrite_args->obj.getAttr(offsetof(BoxedClosure, parent), RewriterVarUsage::NoKill);
                if (rewrite_args->call_done_guarding)
                    rewrite_args->rewriter->setDoneGuarding();
            }
            return getattrInternal(closure->parent, attr, rewrite_args);
        }
        raiseExcHelper(NameError, "free variable '%s' referenced before assignment in enclosing scope", attr.c_str());
    }

    if (!cls_only) {
        // Don't need to pass icentry args, since we special-case __getattribtue__ and __getattr__ to use
        // invalidation rather than guards
        // TODO since you changed this to typeLookup you need to guard
        Box* getattribute = typeLookup(obj->cls, "__getattribute__", NULL);
        if (getattribute) {
            // TODO this is a good candidate for interning?
            Box* boxstr = boxString(attr);
            Box* rtn = runtimeCall2(getattribute, ArgPassSpec(2), obj, boxstr);
            return rtn;
        }

        if (rewrite_args) {
            rewrite_args->rewriter->addDependenceOn(obj->cls->dependent_icgetattrs);
        }
    }

    // Handle descriptor logic here.
    // A descriptor is either a data descriptor or a non-data descriptor.
    // data descriptors define both __get__ and __set__. non-data descriptors
    // only define __get__. Rules are different for the two types, which means
    // that even though __get__ is the one we might call, we still have to check
    // if __set__ exists.
    // If __set__ exists, it's a data descriptor, and it takes precedence over
    // the instance attribute.
    // Otherwise, it's non-data, and we only call __get__ if the instance
    // attribute doesn't exist.

    // In the cls_only case, we ignore the instance attribute
    // (so we don't have to check if __set__ exists at all)

    // Look up the class attribute (called `descr` here because it might
    // be a descriptor).
    Box* descr = NULL;
    RewriterVarUsage r_descr(RewriterVarUsage::empty());
    if (rewrite_args) {
        RewriterVarUsage r_obj_cls
            = rewrite_args->obj.getAttr(BOX_CLS_OFFSET, RewriterVarUsage::NoKill, Location::any());
        GetattrRewriteArgs grewrite_args(rewrite_args->rewriter, std::move(r_obj_cls), rewrite_args->destination,
                                         false);
        descr = typeLookup(obj->cls, attr, &grewrite_args);

        if (!grewrite_args.out_success) {
            rewrite_args = NULL;
        } else if (descr) {
            r_descr = std::move(grewrite_args.out_rtn);
        }
    } else {
        descr = typeLookup(obj->cls, attr, NULL);
    }

    // Check if it's a data descriptor
    Box* _get_ = NULL;
    RewriterVarUsage r_get(RewriterVarUsage::empty());
    if (descr) {
        if (rewrite_args)
            r_descr.addAttrGuard(BOX_CLS_OFFSET, (uint64_t)descr->cls);

        // Special-case data descriptors (e.g., member descriptors)
        Box* res = dataDescriptorInstanceSpecialCases(rewrite_args, obj, descr, r_descr, for_call, should_bind_out);
        if (res) {
            return res;
        }

        // Let's only check if __get__ exists if it's not a special case
        // nondata descriptor. The nondata case is handled below, but
        // we can immediately know to skip this part if it's one of the
        // special case nondata descriptors.
        if (!isNondataDescriptorInstanceSpecialCase(descr)) {
            // Check if __get__ exists
            if (rewrite_args) {
                RewriterVarUsage r_descr_cls
                    = r_descr.getAttr(BOX_CLS_OFFSET, RewriterVarUsage::NoKill, Location::any());
                GetattrRewriteArgs grewrite_args(rewrite_args->rewriter, std::move(r_descr_cls), Location::any(),
                                                 false);
                _get_ = typeLookup(descr->cls, "__get__", &grewrite_args);
                if (!grewrite_args.out_success) {
                    rewrite_args = NULL;
                } else if (_get_) {
                    r_get = std::move(grewrite_args.out_rtn);
                }
            } else {
                _get_ = typeLookup(descr->cls, "__get__", NULL);
            }

            // As an optimization, don't check for __set__ if we're in cls_only mode, since it won't matter.
            if (_get_ && !cls_only) {
                // Check if __set__ exists
                Box* _set_ = NULL;
                if (rewrite_args) {
                    RewriterVarUsage r_descr_cls
                        = r_descr.getAttr(BOX_CLS_OFFSET, RewriterVarUsage::NoKill, Location::any());
                    GetattrRewriteArgs grewrite_args(rewrite_args->rewriter, std::move(r_descr_cls), Location::any(),
                                                     false);
                    _set_ = typeLookup(descr->cls, "__set__", &grewrite_args);
                    if (!grewrite_args.out_success) {
                        rewrite_args = NULL;
                    } else if (_set_) {
                        grewrite_args.out_rtn.setDoneUsing();
                    }
                } else {
                    _set_ = typeLookup(descr->cls, "__set__", NULL);
                }

                // Call __get__(descr, obj, obj->cls)
                if (_set_) {
                    // this could happen for the callattr path...
                    if (rewrite_args && !rewrite_args->call_done_guarding)
                        rewrite_args = NULL;

                    Box* res;
                    if (rewrite_args) {
                        CallRewriteArgs crewrite_args(rewrite_args->rewriter, std::move(r_get),
                                                      rewrite_args->destination, rewrite_args->call_done_guarding);
                        crewrite_args.arg1 = std::move(r_descr);
                        crewrite_args.arg2 = rewrite_args->obj.addUse();
                        crewrite_args.arg3
                            = rewrite_args->obj.getAttr(BOX_CLS_OFFSET, RewriterVarUsage::Kill, Location::any());
                        res = runtimeCallInternal(_get_, &crewrite_args, ArgPassSpec(3), descr, obj, obj->cls, NULL,
                                                  NULL);
                        if (!crewrite_args.out_success) {
                            rewrite_args = NULL;
                        } else {
                            rewrite_args->out_success = true;
                            rewrite_args->out_rtn = std::move(crewrite_args.out_rtn);
                        }
                    } else {
                        r_descr.ensureDoneUsing();
                        r_get.ensureDoneUsing();
                        res = runtimeCallInternal(_get_, NULL, ArgPassSpec(3), descr, obj, obj->cls, NULL, NULL);
                    }
                    return res;
                }
            }
        }
    }

    if (!cls_only) {
        if (obj->cls != type_cls) {
            // Look up the val in the object's dictionary and if you find it, return it.

            Box* val;
            RewriterVarUsage r_val(RewriterVarUsage::empty());
            if (rewrite_args) {
                GetattrRewriteArgs hrewrite_args(rewrite_args->rewriter, rewrite_args->obj.addUse(),
                                                 rewrite_args->destination, false);
                val = obj->getattr(attr, &hrewrite_args);

                if (!hrewrite_args.out_success) {
                    rewrite_args = NULL;
                } else if (val) {
                    r_val = std::move(hrewrite_args.out_rtn);
                }
            } else {
                val = obj->getattr(attr, NULL);
            }

            if (val) {
                if (rewrite_args) {
                    if (rewrite_args->call_done_guarding)
                        rewrite_args->rewriter->setDoneGuarding();
                    rewrite_args->obj.setDoneUsing();
                    rewrite_args->out_rtn = std::move(r_val);
                    rewrite_args->out_success = true;
                }
                r_descr.ensureDoneUsing();
                r_get.ensureDoneUsing();
                return val;
            }
        } else {
            // More complicated when obj is a type
            // We have to look up the attr in the entire
            // class hierarchy, and we also have to check if it is a descriptor,
            // in addition to the data/nondata descriptor logic.
            // (in CPython, see type_getattro in typeobject.c)

            Box* val;
            RewriterVarUsage r_val(RewriterVarUsage::empty());
            if (rewrite_args) {
                GetattrRewriteArgs grewrite_args(rewrite_args->rewriter, rewrite_args->obj.addUse(),
                                                 rewrite_args->destination, false);

                val = typeLookup(static_cast<BoxedClass*>(obj), attr, &grewrite_args);
                if (!grewrite_args.out_success) {
                    rewrite_args = NULL;
                } else if (val) {
                    r_val = std::move(grewrite_args.out_rtn);
                }
            } else {
                val = typeLookup(static_cast<BoxedClass*>(obj), attr, NULL);
            }

            if (val) {
                r_get.ensureDoneUsing();
                r_descr.ensureDoneUsing();

                Box* res = descriptorClsSpecialCases(rewrite_args, static_cast<BoxedClass*>(obj), val, r_val, for_call,
                                                     should_bind_out);
                if (res) {
                    return res;
                }

                // Lookup __get__
                RewriterVarUsage r_get(RewriterVarUsage::empty());
                Box* local_get;
                if (rewrite_args) {
                    RewriterVarUsage r_val_cls
                        = r_val.getAttr(BOX_CLS_OFFSET, RewriterVarUsage::NoKill, Location::any());
                    GetattrRewriteArgs grewrite_args(rewrite_args->rewriter, std::move(r_val_cls), Location::any(),
                                                     false);
                    local_get = typeLookup(val->cls, "__get__", &grewrite_args);
                    if (!grewrite_args.out_success) {
                        rewrite_args = NULL;
                    } else if (local_get) {
                        r_get = std::move(grewrite_args.out_rtn);
                    }
                } else {
                    local_get = typeLookup(val->cls, "__get__", NULL);
                }

                // Call __get__(val, None, obj)
                if (local_get) {
                    Box* res;

                    // this could happen for the callattr path...
                    if (rewrite_args && !rewrite_args->call_done_guarding)
                        rewrite_args = NULL;

                    if (rewrite_args) {
                        CallRewriteArgs crewrite_args(rewrite_args->rewriter, std::move(r_get),
                                                      rewrite_args->destination, rewrite_args->call_done_guarding);
                        crewrite_args.arg1 = std::move(r_val);
                        crewrite_args.arg2 = rewrite_args->rewriter->loadConst((intptr_t)None, Location::any());
                        crewrite_args.arg3 = std::move(rewrite_args->obj);
                        res = runtimeCallInternal(local_get, &crewrite_args, ArgPassSpec(3), val, None, obj, NULL,
                                                  NULL);
                        if (!crewrite_args.out_success) {
                            rewrite_args = NULL;
                        } else {
                            rewrite_args->out_success = true;
                            rewrite_args->out_rtn = std::move(crewrite_args.out_rtn);
                        }
                    } else {
                        r_val.ensureDoneUsing();
                        r_get.ensureDoneUsing();
                        res = runtimeCallInternal(local_get, NULL, ArgPassSpec(3), val, None, obj, NULL, NULL);
                    }
                    return res;
                }

                // If there was no local __get__, just return val
                if (rewrite_args) {
                    if (rewrite_args->call_done_guarding)
                        rewrite_args->rewriter->setDoneGuarding();
                    rewrite_args->obj.setDoneUsing();
                    rewrite_args->out_rtn = std::move(r_val);
                    rewrite_args->out_success = true;
                } else {
                    r_val.ensureDoneUsing();
                }
                return val;
            }
        }
    }

    // If descr and __get__ exist, then call __get__
    if (descr) {
        // Special cases first
        Box* res = nondataDescriptorInstanceSpecialCases(rewrite_args, obj, descr, r_descr, for_call, should_bind_out);
        if (res) {
            return res;
        }

        // We looked up __get__ above. If we found it, call it and return
        // the result.
        if (_get_) {
            // this could happen for the callattr path...
            if (rewrite_args && !rewrite_args->call_done_guarding)
                rewrite_args = NULL;

            Box* res;
            if (rewrite_args) {
                CallRewriteArgs crewrite_args(rewrite_args->rewriter, std::move(r_get), rewrite_args->destination,
                                              rewrite_args->call_done_guarding);
                crewrite_args.arg1 = std::move(r_descr);
                crewrite_args.arg2 = rewrite_args->obj.addUse();
                crewrite_args.arg3 = rewrite_args->obj.getAttr(BOX_CLS_OFFSET, RewriterVarUsage::Kill, Location::any());
                res = runtimeCallInternal(_get_, &crewrite_args, ArgPassSpec(3), descr, obj, obj->cls, NULL, NULL);
                if (!crewrite_args.out_success) {
                    rewrite_args = NULL;
                } else {
                    rewrite_args->out_success = true;
                    rewrite_args->out_rtn = std::move(crewrite_args.out_rtn);
                }
            } else {
                r_descr.ensureDoneUsing();
                r_get.ensureDoneUsing();
                res = runtimeCallInternal(_get_, NULL, ArgPassSpec(3), descr, obj, obj->cls, NULL, NULL);
            }
            return res;
        }

        // Otherwise, just return descr.
        if (rewrite_args) {
            rewrite_args->obj.setDoneUsing();
            if (rewrite_args->call_done_guarding)
                rewrite_args->rewriter->setDoneGuarding();
            rewrite_args->out_rtn = std::move(r_descr);
            rewrite_args->out_success = true;
        }
        return descr;
    }

    // Finally, check __getattr__

    if (!cls_only) {
        // Don't need to pass icentry args, since we special-case __getattribute__ and __getattr__ to use
        // invalidation rather than guards
        rewrite_args = NULL;
        Box* getattr = typeLookup(obj->cls, "__getattr__", NULL);
        if (getattr) {
            Box* boxstr = boxString(attr);
            Box* rtn = runtimeCall2(getattr, ArgPassSpec(2), obj, boxstr);

            if (rewrite_args)
                rewrite_args->out_rtn.ensureDoneUsing();
            return rtn;
        }

        if (rewrite_args) {
            rewrite_args->rewriter->addDependenceOn(obj->cls->dependent_icgetattrs);
        }
    }

    if (rewrite_args) {
        rewrite_args->obj.ensureDoneUsing();
        rewrite_args->out_success = true;
    }
    return NULL;
}

Box* getattrInternal(Box* obj, const std::string& attr, GetattrRewriteArgs* rewrite_args) {
    return getattrInternalGeneral(obj, attr, rewrite_args,
                                  /* cls_only */ false,
                                  /* for_call */ false, NULL);
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

    if (strcmp(attr, "__dict__") == 0) {
        if (obj->cls->instancesHaveAttrs())
            return makeAttrWrapper(obj);
    }

    std::unique_ptr<Rewriter> rewriter(
        Rewriter::createRewriter(__builtin_extract_return_addr(__builtin_return_address(0)), 2, "getattr"));

    Box* val;
    if (rewriter.get()) {
        Location dest;
        TypeRecorder* recorder = rewriter->getTypeRecorder();
        if (recorder)
            dest = Location::forArg(1);
        else
            dest = rewriter->getReturnDestination();
        GetattrRewriteArgs rewrite_args(rewriter.get(), rewriter->getArg(0), dest, true);
        val = getattrInternal(obj, attr, &rewrite_args);

        // should make sure getattrInternal calls finishes using obj itself
        // if it is successful
        if (!rewrite_args.out_success)
            rewrite_args.obj.ensureDoneUsing();

        if (rewrite_args.out_success && val) {
            if (recorder) {
                RewriterVarUsage record_rtn = rewriter->call(
                    false, (void*)recordType, rewriter->loadConst((intptr_t)recorder, Location::forArg(0)),
                    std::move(rewrite_args.out_rtn));
                rewriter->commitReturning(std::move(record_rtn));

                recordType(recorder, val);
            } else {
                rewriter->commitReturning(std::move(rewrite_args.out_rtn));
            }
        }
    } else {
        val = getattrInternal(obj, attr, NULL);
    }

    if (val) {
        return val;
    }
    raiseAttributeError(obj, attr);
}

void setattrInternal(Box* obj, const std::string& attr, Box* val, SetattrRewriteArgs* rewrite_args) {
    assert(gc::isValidGCObject(val));

    // Lookup a descriptor
    Box* descr = NULL;
    RewriterVarUsage r_descr(RewriterVarUsage::empty());
    // TODO probably check that the cls is user-defined or something like that
    // (figure out exactly what)
    // (otherwise no need to check descriptor logic)
    if (rewrite_args) {
        RewriterVarUsage r_cls
            = rewrite_args->obj.getAttr(BOX_CLS_OFFSET, RewriterVarUsage::KillFlag::NoKill, Location::any());
        GetattrRewriteArgs crewrite_args(rewrite_args->rewriter, std::move(r_cls),
                                         rewrite_args->rewriter->getReturnDestination(), false);
        descr = typeLookup(obj->cls, attr, &crewrite_args);

        if (!crewrite_args.out_success) {
            rewrite_args = NULL;
        } else if (descr) {
            r_descr = std::move(crewrite_args.out_rtn);
        }
    } else {
        descr = typeLookup(obj->cls, attr, NULL);
    }

    Box* _set_ = NULL;
    RewriterVarUsage r_set(RewriterVarUsage::empty());
    if (descr) {
        if (rewrite_args) {
            RewriterVarUsage r_cls
                = r_descr.getAttr(BOX_CLS_OFFSET, RewriterVarUsage::KillFlag::NoKill, Location::any());
            GetattrRewriteArgs trewrite_args(rewrite_args->rewriter, std::move(r_cls), Location::any(), false);
            _set_ = typeLookup(descr->cls, "__set__", &trewrite_args);
            if (!trewrite_args.out_success) {
                rewrite_args = NULL;
            } else if (_set_) {
                r_set = std::move(trewrite_args.out_rtn);
            }
        } else {
            _set_ = typeLookup(descr->cls, "__set__", NULL);
        }
    }

    // If `descr` has __set__ (thus making it a descriptor) we should call
    // __set__ with `val` rather than directly calling setattr
    if (descr && _set_) {
        if (rewrite_args) {
            CallRewriteArgs crewrite_args(rewrite_args->rewriter, std::move(r_set), Location::any(),
                                          rewrite_args->call_done_guarding);
            crewrite_args.arg1 = std::move(r_descr);
            crewrite_args.arg2 = std::move(rewrite_args->obj);
            crewrite_args.arg3 = std::move(rewrite_args->attrval);
            runtimeCallInternal(_set_, &crewrite_args, ArgPassSpec(3), descr, obj, val, NULL, NULL);
            if (crewrite_args.out_success) {
                crewrite_args.out_rtn.setDoneUsing();
                rewrite_args->out_success = true;
            }
        } else {
            runtimeCallInternal(_set_, NULL, ArgPassSpec(3), descr, obj, val, NULL, NULL);
        }
    } else {
        r_descr.ensureDoneUsing();
        r_set.ensureDoneUsing();
        obj->setattr(attr, val, rewrite_args);
    }
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

    std::unique_ptr<Rewriter> rewriter(
        Rewriter::createRewriter(__builtin_extract_return_addr(__builtin_return_address(0)), 3, "setattr"));

    if (rewriter.get()) {
        // rewriter->trap();
        SetattrRewriteArgs rewrite_args(rewriter.get(), rewriter->getArg(0), rewriter->getArg(2), true);
        setattrInternal(obj, attr, attr_val, &rewrite_args);
        if (rewrite_args.out_success) {
            rewriter->commit();
        } else {
            rewrite_args.obj.ensureDoneUsing();
            rewrite_args.attrval.ensureDoneUsing();
        }
    } else {
        setattrInternal(obj, attr, attr_val, NULL);
    }
}

bool isUserDefined(BoxedClass* cls) {
    return cls->is_user_defined;
    // return cls->hasattrs && (cls != function_cls && cls != type_cls) && !cls->is_constant;
}

extern "C" bool nonzero(Box* obj) {
    static StatCounter slowpath_nonzero("slowpath_nonzero");

    std::unique_ptr<Rewriter> rewriter(
        Rewriter::createRewriter(__builtin_extract_return_addr(__builtin_return_address(0)), 1, "nonzero"));

    RewriterVarUsage r_obj(RewriterVarUsage::empty());
    if (rewriter.get()) {
        r_obj = std::move(rewriter->getArg(0));
        // rewriter->trap();
        r_obj.addAttrGuard(BOX_CLS_OFFSET, (intptr_t)obj->cls);
        rewriter->setDoneGuarding();
    }

    if (obj->cls == bool_cls) {
        if (rewriter.get()) {
            RewriterVarUsage b
                = r_obj.getAttr(BOOL_B_OFFSET, RewriterVarUsage::KillFlag::Kill, rewriter->getReturnDestination());
            rewriter->commitReturning(std::move(b));
        }

        BoxedBool* bool_obj = static_cast<BoxedBool*>(obj);
        return bool_obj->b;
    } else if (obj->cls == int_cls) {
        if (rewriter.get()) {
            // TODO should do:
            // test 	%rsi, %rsi
            // setne	%al
            RewriterVarUsage n
                = r_obj.getAttr(INT_N_OFFSET, RewriterVarUsage::KillFlag::Kill, rewriter->getReturnDestination());
            RewriterVarUsage b = n.toBool(RewriterVarUsage::KillFlag::Kill, rewriter->getReturnDestination());
            rewriter->commitReturning(std::move(b));
        }

        BoxedInt* int_obj = static_cast<BoxedInt*>(obj);
        return int_obj->n != 0;
    } else if (obj->cls == float_cls) {
        if (rewriter.get()) {
            RewriterVarUsage b = rewriter->call(false, (void*)floatNonzeroUnboxed, std::move(r_obj));
            rewriter->commitReturning(std::move(b));
        }
        return static_cast<BoxedFloat*>(obj)->d != 0;
    } else if (obj->cls == none_cls) {
        if (rewriter.get()) {
            r_obj.setDoneUsing();
            RewriterVarUsage b = rewriter->loadConst(0, rewriter->getReturnDestination());
            rewriter->commitReturning(std::move(b));
        }
        return false;
    }

    if (rewriter.get()) {
        r_obj.setDoneUsing();
    }

    // FIXME we have internal functions calling this method;
    // instead, we should break this out into an external and internal function.
    // slowpath_* counters are supposed to count external calls; putting it down
    // here gets a better representation of that.
    // TODO move internal callers to nonzeroInternal, and log *all* calls to nonzero
    slowpath_nonzero.log();

    // int id = Stats::getStatId("slowpath_nonzero_" + *getTypeName(obj));
    // Stats::log(id);

    // go through descriptor logic
    Box* func = getclsattr_internal(obj, "__nonzero__", NULL);

    if (func == NULL) {
        ASSERT(isUserDefined(obj->cls) || obj->cls == classobj_cls, "%s.__nonzero__",
               getTypeName(obj)->c_str()); // TODO
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
        static const std::string str_str("__str__");
        obj = callattrInternal(obj, &str_str, CLASS_ONLY, NULL, ArgPassSpec(0), NULL, NULL, NULL, NULL, NULL);
    }

    if (obj->cls != str_cls) {
        raiseExcHelper(TypeError, "__str__ did not return a string!");
    }
    return static_cast<BoxedString*>(obj);
}

extern "C" BoxedString* repr(Box* obj) {
    static StatCounter slowpath_repr("slowpath_repr");
    slowpath_repr.log();

    static const std::string repr_str("__repr__");
    obj = callattrInternal(obj, &repr_str, CLASS_ONLY, NULL, ArgPassSpec(0), NULL, NULL, NULL, NULL, NULL);

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

    if (cls->cls == tuple_cls) {
        auto t = static_cast<BoxedTuple*>(cls);
        for (auto c : t->elts) {
            if (isinstance(obj, c, flags))
                return true;
        }
        return false;
    }

    if (cls->cls == classobj_cls) {
        if (!isSubclass(obj->cls, instance_cls))
            return false;

        return instanceIsinstance(static_cast<BoxedInstance*>(obj), static_cast<BoxedClassobj*>(cls));
    }

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

    // goes through descriptor logic
    Box* hash = getclsattr_internal(obj, "__hash__", NULL);

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
        CallRewriteArgs crewrite_args(rewrite_args->rewriter, std::move(rewrite_args->obj), rewrite_args->destination,
                                      rewrite_args->call_done_guarding);
        rtn = callattrInternal0(obj, &attr_str, CLASS_ONLY, &crewrite_args, ArgPassSpec(0));
        if (!crewrite_args.out_success)
            rewrite_args = NULL;
        else if (rtn)
            rewrite_args->out_rtn = std::move(crewrite_args.out_rtn);
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
        Rewriter::createRewriter(__builtin_extract_return_addr(__builtin_return_address(0)), 1, "unboxedLen"));

    BoxedInt* lobj;
    RewriterVarUsage r_boxed(RewriterVarUsage::empty());
    if (rewriter.get()) {
        // rewriter->trap();
        LenRewriteArgs rewrite_args(rewriter.get(), rewriter->getArg(0), rewriter->getReturnDestination(), true);
        lobj = lenInternal(obj, &rewrite_args);

        if (!rewrite_args.out_success) {
            rewrite_args.ensureAllDone();
            rewriter.reset(NULL);
        } else
            r_boxed = std::move(rewrite_args.out_rtn);
    } else {
        lobj = lenInternal(obj, NULL);
    }

    assert(lobj->cls == int_cls);
    i64 rtn = lobj->n;

    if (rewriter.get()) {
        RewriterVarUsage rtn
            = std::move(r_boxed.getAttr(INT_N_OFFSET, RewriterVarUsage::KillFlag::Kill, Location(assembler::RAX)));
        rewriter->commitReturning(std::move(rtn));
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

    gc::GCAllocation* al = gc::GCAllocation::fromUserData(p);
    if (al->kind_id == gc::GCKind::UNTRACKED) {
        printf("gc-untracked object\n");
        return;
    }

    if (al->kind_id == gc::GCKind::CONSERVATIVE) {
        printf("conservatively-scanned object object\n");
        return;
    }

    if (al->kind_id == gc::GCKind::PYTHON) {
        printf("Python object\n");
        Box* b = (Box*)p;
        printf("Class: %s\n", getFullTypeName(b).c_str());

        if (isSubclass(b->cls, type_cls)) {
            printf("Type name: %s\n", getFullNameOfClass(static_cast<BoxedClass*>(b)).c_str());
        }

        if (isSubclass(b->cls, str_cls)) {
            printf("String value: %s\n", static_cast<BoxedString*>(b)->s.c_str());
        }
        return;
    }

    RELEASE_ASSERT(0, "%d", (int)al->kind_id);
}

// For rewriting purposes, this function assumes that nargs will be constant.
// That's probably fine for some uses (ex binops), but otherwise it should be guarded on beforehand.
extern "C" Box* callattrInternal(Box* obj, const std::string* attr, LookupScope scope, CallRewriteArgs* rewrite_args,
                                 ArgPassSpec argspec, Box* arg1, Box* arg2, Box* arg3, Box** args,
                                 const std::vector<const std::string*>* keyword_names) {
    int npassed_args = argspec.totalPassed();

    if (rewrite_args && !rewrite_args->args_guarded) {
        // TODO duplication with runtime_call
        // TODO should know which args don't need to be guarded, ex if we're guaranteed that they
        // already fit, either since the type inferencer could determine that,
        // or because they only need to fit into an UNKNOWN slot.

        if (npassed_args >= 1)
            rewrite_args->arg1.addAttrGuard(BOX_CLS_OFFSET, (intptr_t)arg1->cls);
        if (npassed_args >= 2)
            rewrite_args->arg2.addAttrGuard(BOX_CLS_OFFSET, (intptr_t)arg2->cls);
        if (npassed_args >= 3)
            rewrite_args->arg3.addAttrGuard(BOX_CLS_OFFSET, (intptr_t)arg3->cls);

        if (npassed_args > 3) {
            for (int i = 3; i < npassed_args; i++) {
                // TODO if there are a lot of args (>16), might be better to increment a pointer
                // rather index them directly?
                RewriterVarUsage v = rewrite_args->args.getAttr((i - 3) * sizeof(Box*),
                                                                RewriterVarUsage::KillFlag::NoKill, Location::any());
                v.addAttrGuard(BOX_CLS_OFFSET, (intptr_t)args[i - 3]->cls);
                v.setDoneUsing();
            }
        }
    }

    // right now I don't think this is ever called with INST_ONLY?
    assert(scope != INST_ONLY);

    // Look up the argument. Pass in the arguments to getattrInternalGeneral or getclsattr_general
    // that will shortcut functions by not putting them into instancemethods
    bool should_bind;
    Box* val;
    RewriterVarUsage r_val(RewriterVarUsage::empty());
    if (rewrite_args) {
        GetattrRewriteArgs grewrite_args(rewrite_args->rewriter, rewrite_args->obj.addUse(), Location::any(), false);
        val = getattrInternalGeneral(obj, *attr, &grewrite_args, scope == CLASS_ONLY, true, &should_bind);
        if (!grewrite_args.out_success) {
            rewrite_args = NULL;
        } else if (val) {
            r_val = std::move(grewrite_args.out_rtn);
        }
    } else {
        val = getattrInternalGeneral(obj, *attr, NULL, scope == CLASS_ONLY, true, &should_bind);
    }

    if (val == NULL) {
        if (rewrite_args) {
            rewrite_args->arg1.ensureDoneUsing();
            rewrite_args->arg2.ensureDoneUsing();
            rewrite_args->arg3.ensureDoneUsing();
            rewrite_args->args.ensureDoneUsing();
            rewrite_args->out_success = true;
            rewrite_args->obj.setDoneUsing();
        }
        return val;
    }

    if (should_bind) {
        if (rewrite_args) {
            r_val.addGuard((int64_t)val);
        }

        // TODO copy from runtimeCall
        // TODO these two branches could probably be folded together (the first one is becoming
        // a subset of the second)
        if (npassed_args <= 2) {
            Box* rtn;
            if (rewrite_args) {
                CallRewriteArgs srewrite_args(rewrite_args->rewriter, std::move(r_val), rewrite_args->destination,
                                              rewrite_args->call_done_guarding);
                srewrite_args.arg1 = std::move(rewrite_args->obj);

                // should be no-ops:
                if (npassed_args >= 1)
                    srewrite_args.arg2 = std::move(rewrite_args->arg1);
                if (npassed_args >= 2)
                    srewrite_args.arg3 = std::move(rewrite_args->arg2);

                srewrite_args.func_guarded = true;
                srewrite_args.args_guarded = true;

                rtn = runtimeCallInternal(val, &srewrite_args, ArgPassSpec(argspec.num_args + 1, argspec.num_keywords,
                                                                           argspec.has_starargs, argspec.has_kwargs),
                                          obj, arg1, arg2, NULL, keyword_names);

                if (!srewrite_args.out_success) {
                    rewrite_args = NULL;
                } else {
                    rewrite_args->out_rtn = std::move(srewrite_args.out_rtn);
                }
            } else {
                rtn = runtimeCallInternal(val, NULL, ArgPassSpec(argspec.num_args + 1, argspec.num_keywords,
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
                CallRewriteArgs srewrite_args(rewrite_args->rewriter, std::move(r_val), rewrite_args->destination,
                                              rewrite_args->call_done_guarding);
                srewrite_args.arg1 = std::move(rewrite_args->obj);
                srewrite_args.arg2 = std::move(rewrite_args->arg1);
                srewrite_args.arg3 = std::move(rewrite_args->arg2);
                srewrite_args.args = rewrite_args->rewriter->allocateAndCopyPlus1(
                    std::move(rewrite_args->arg3),
                    npassed_args == 3 ? RewriterVarUsage::empty() : std::move(rewrite_args->args), npassed_args - 3);

                srewrite_args.args_guarded = true;
                srewrite_args.func_guarded = true;

                rtn = runtimeCallInternal(val, &srewrite_args, ArgPassSpec(argspec.num_args + 1, argspec.num_keywords,
                                                                           argspec.has_starargs, argspec.has_kwargs),
                                          obj, arg1, arg2, new_args, keyword_names);

                if (!srewrite_args.out_success) {
                    rewrite_args = NULL;
                } else {
                    rewrite_args->out_rtn = std::move(srewrite_args.out_rtn);

                    rewrite_args->out_success = true;
                }
            } else {
                rtn = runtimeCallInternal(val, NULL, ArgPassSpec(argspec.num_args + 1, argspec.num_keywords,
                                                                 argspec.has_starargs, argspec.has_kwargs),
                                          obj, arg1, arg2, new_args, keyword_names);
            }
            return rtn;
        }
    } else {
        if (rewrite_args) {
            rewrite_args->obj.setDoneUsing();
        }

        Box* rtn;
        if (val->cls != function_cls && val->cls != instancemethod_cls) {
            rewrite_args = NULL;
            r_val.ensureDoneUsing();
        }

        if (rewrite_args) {
            CallRewriteArgs srewrite_args(rewrite_args->rewriter, std::move(r_val), rewrite_args->destination,
                                          rewrite_args->call_done_guarding);
            if (npassed_args >= 1)
                srewrite_args.arg1 = std::move(rewrite_args->arg1);
            if (npassed_args >= 2)
                srewrite_args.arg2 = std::move(rewrite_args->arg2);
            if (npassed_args >= 3)
                srewrite_args.arg3 = std::move(rewrite_args->arg3);
            if (npassed_args >= 4)
                srewrite_args.args = std::move(rewrite_args->args);
            srewrite_args.args_guarded = true;

            rtn = runtimeCallInternal(val, &srewrite_args, argspec, arg1, arg2, arg3, args, keyword_names);

            if (!srewrite_args.out_success) {
                rewrite_args = NULL;
            } else {
                rewrite_args->out_rtn = std::move(srewrite_args.out_rtn);
            }
        } else {
            rtn = runtimeCallInternal(val, NULL, argspec, arg1, arg2, arg3, args, keyword_names);
        }

        if (!rtn) {
            raiseExcHelper(TypeError, "'%s' object is not callable", getTypeName(val)->c_str());
        }

        if (rewrite_args)
            rewrite_args->out_success = true;
        return rtn;
    }
}

extern "C" Box* callattr(Box* obj, const std::string* attr, bool clsonly, ArgPassSpec argspec, Box* arg1, Box* arg2,
                         Box* arg3, Box** args, const std::vector<const std::string*>* keyword_names) {
    int npassed_args = argspec.totalPassed();

    static StatCounter slowpath_callattr("slowpath_callattr");
    slowpath_callattr.log();

    assert(attr);

    int num_orig_args = 4 + std::min(4, npassed_args);
    if (argspec.num_keywords)
        num_orig_args++;
    std::unique_ptr<Rewriter> rewriter(Rewriter::createRewriter(
        __builtin_extract_return_addr(__builtin_return_address(0)), num_orig_args, "callattr"));
    Box* rtn;

    LookupScope scope = clsonly ? CLASS_ONLY : CLASS_OR_INST;

    if (rewriter.get()) {
        // TODO feel weird about doing this; it either isn't necessary
        // or this kind of thing is necessary in a lot more places
        // rewriter->getArg(3).addGuard(npassed_args);

        CallRewriteArgs rewrite_args(rewriter.get(), std::move(rewriter->getArg(0)), rewriter->getReturnDestination(),
                                     true);
        if (npassed_args >= 1)
            rewrite_args.arg1 = std::move(rewriter->getArg(4));
        if (npassed_args >= 2)
            rewrite_args.arg2 = std::move(rewriter->getArg(5));
        if (npassed_args >= 3)
            rewrite_args.arg3 = std::move(rewriter->getArg(6));
        if (npassed_args >= 4)
            rewrite_args.args = std::move(rewriter->getArg(7));
        rtn = callattrInternal(obj, attr, scope, &rewrite_args, argspec, arg1, arg2, arg3, args, keyword_names);

        if (!rewrite_args.out_success) {
            rewrite_args.ensureAllDone();
            rewriter.reset(NULL);
        } else if (rtn) {
            rewriter->commitReturning(std::move(rewrite_args.out_rtn));
        }
    } else {
        rtn = callattrInternal(obj, attr, scope, NULL, argspec, arg1, arg2, arg3, args, keyword_names);
    }

    if (rtn == NULL) {
        raiseAttributeError(obj, attr->c_str());
    }

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
            Box*& v = okwargs->d[boxString(kw_name)];
            if (v) {
                raiseExcHelper(TypeError, "<function>() got multiple values for keyword argument '%s'",
                               kw_name.c_str());
            }
            v = kw_val;
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

    if (argspec.has_starargs || argspec.has_kwargs || f->takes_kwargs || func->isGenerator) {
        rewrite_args = NULL;
    }

    // These could be handled:
    if (argspec.num_keywords) {
        rewrite_args = NULL;
    }

    // TODO Should we guard on the CLFunction or the BoxedFunction?
    // A single CLFunction could end up forming multiple BoxedFunctions, and we
    // could emit assembly that handles any of them.  But doing this involves some
    // extra indirection, and it's not clear if that's worth it, since it seems like
    // the common case will be functions only ever getting a single set of default arguments.
    bool guard_clfunc = false;
    assert(!guard_clfunc && "I think there are users that expect the boxedfunction to be guarded");

    if (rewrite_args) {
        assert(rewrite_args->args_guarded && "need to guard args here");

        if (!rewrite_args->func_guarded) {
            if (guard_clfunc) {
                rewrite_args->obj.addAttrGuard(offsetof(BoxedFunction, f), (intptr_t)f);
            } else {
                rewrite_args->obj.addGuard((intptr_t)func);
                rewrite_args->obj.setDoneUsing();
            }
        } else {
            rewrite_args->obj.setDoneUsing();
        }
        assert(rewrite_args->call_done_guarding);
        if (!rewrite_args->rewriter->isDoneGuarding())
            rewrite_args->rewriter->setDoneGuarding();
        // if (guard_clfunc) {
        // Have to save the defaults array since the object itself will probably get overwritten:
        // rewrite_args->obj = rewrite_args->obj.move(-2);
        // r_defaults_array = rewrite_args->obj.getAttr(offsetof(BoxedFunction, defaults), -2);
        //}
    }

    if (rewrite_args) {
        // We might have trouble if we have more output args than input args,
        // such as if we need more space to pass defaults.
        if (num_output_args > 3 && num_output_args > argspec.totalPassed()) {
            int arg_bytes_required = (num_output_args - 3) * sizeof(Box*);
            RewriterVarUsage new_args(RewriterVarUsage::empty());
            if (rewrite_args->args.isDoneUsing()) {
                // rewrite_args->args could be empty if there are not more than
                // 3 input args.
                new_args = rewrite_args->rewriter->allocate(num_output_args - 3);
            } else {
                new_args = rewrite_args->rewriter->allocateAndCopy(std::move(rewrite_args->args), num_output_args - 3);
            }

            rewrite_args->args = std::move(new_args);
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

    if (num_output_args > 3) {
        int size = (num_output_args - 3) * sizeof(Box*);
        oargs = (Box**)alloca(size);

#ifndef NDEBUG
        memset(&oargs[0], 0, size);
#endif
    }

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

    std::vector<bool> params_filled(num_output_args, false);
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
            // rewrite_args->rewriter->loadConst((intptr_t)EmptyTuple, Location::forArg(varargs_idx));
            RewriterVarUsage emptyTupleConst = rewrite_args->rewriter->loadConst(
                (intptr_t)EmptyTuple, varargs_idx < 3 ? Location::forArg(varargs_idx) : Location::any());
            if (varargs_idx == 0)
                rewrite_args->arg1 = std::move(emptyTupleConst);
            if (varargs_idx == 1)
                rewrite_args->arg2 = std::move(emptyTupleConst);
            if (varargs_idx == 2)
                rewrite_args->arg3 = std::move(emptyTupleConst);
            if (varargs_idx >= 3)
                rewrite_args->args.setAttr((varargs_idx - 3) * sizeof(Box*), std::move(emptyTupleConst));
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
    if (arg_names == nullptr && argspec.num_keywords && !f->takes_kwargs) {
        raiseExcHelper(TypeError, "<function @%p>() doesn't take keyword arguments", f->versions[0]->code);
    }

    if (argspec.num_keywords)
        assert(argspec.num_keywords == keyword_names->size());

    for (int i = 0; i < argspec.num_keywords; i++) {
        assert(!rewrite_args && "would need to be handled here");

        int arg_idx = i + argspec.num_args;
        Box* kw_val = getArg(arg_idx, arg1, arg2, arg3, args);

        if (!arg_names) {
            assert(okwargs);
            okwargs->d[boxStringPtr((*keyword_names)[i])] = kw_val;
            continue;
        }

        assert(arg_names);

        placeKeyword(*arg_names, params_filled, *(*keyword_names)[i], kw_val, oarg1, oarg2, oarg3, oargs, okwargs);
    }

    if (argspec.has_kwargs) {
        assert(!rewrite_args && "would need to be handled here");

        Box* kwargs
            = getArg(argspec.num_args + argspec.num_keywords + (argspec.has_starargs ? 1 : 0), arg1, arg2, arg3, args);
        RELEASE_ASSERT(kwargs->cls == dict_cls, "haven't implemented this for non-dicts");
        BoxedDict* d_kwargs = static_cast<BoxedDict*>(kwargs);

        for (auto& p : d_kwargs->d) {
            if (p.first->cls != str_cls)
                raiseExcHelper(TypeError, "<function>() keywords must be strings");

            BoxedString* s = static_cast<BoxedString*>(p.first);

            if (arg_names) {
                placeKeyword(*arg_names, params_filled, s->s, p.second, oarg1, oarg2, oarg3, oargs, okwargs);
            } else {
                assert(okwargs);

                Box*& v = okwargs->d[p.first];
                if (v) {
                    raiseExcHelper(TypeError, "<function>() got multiple values for keyword argument '%s'",
                                   s->s.c_str());
                }
                v = p.second;
            }
        }
    }

    // Fill with defaults:

    for (int i = 0; i < f->num_args - f->num_defaults; i++) {
        if (params_filled[i])
            continue;
        // TODO not right error message
        raiseExcHelper(TypeError, "<function>() did not get a value for positional argument %d", i);
    }

    RewriterVarUsage r_defaults_array(RewriterVarUsage::empty());
    if (guard_clfunc) {
        r_defaults_array = rewrite_args->obj.getAttr(offsetof(BoxedFunction, defaults),
                                                     RewriterVarUsage::KillFlag::Kill, Location::any());
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
                    RewriterVarUsage r_default
                        = r_defaults_array.getAttr(offset, RewriterVarUsage::KillFlag::NoKill, Location::forArg(i));
                    if (i == 0)
                        rewrite_args->arg1 = std::move(r_default);
                    if (i == 1)
                        rewrite_args->arg2 = std::move(r_default);
                    if (i == 2)
                        rewrite_args->arg3 = std::move(r_default);
                } else {
                    RewriterVarUsage r_default
                        = r_defaults_array.getAttr(offset, RewriterVarUsage::KillFlag::Kill, Location::any());
                    rewrite_args->args.setAttr((i - 3) * sizeof(Box*), std::move(r_default));
                }
            } else {
                // If we guarded on the BoxedFunction, which has a constant set of defaults,
                // we can embed the default arguments directly into the instructions.
                if (i < 3) {
                    RewriterVarUsage r_default
                        = rewrite_args->rewriter->loadConst((intptr_t)default_obj, Location::any());
                    if (i == 0)
                        rewrite_args->arg1 = std::move(r_default);
                    if (i == 1)
                        rewrite_args->arg2 = std::move(r_default);
                    if (i == 2)
                        rewrite_args->arg3 = std::move(r_default);
                } else {
                    RewriterVarUsage r_default
                        = rewrite_args->rewriter->loadConst((intptr_t)default_obj, Location::any());
                    rewrite_args->args.setAttr((i - 3) * sizeof(Box*), std::move(r_default));
                }
            }
        }

        getArg(i, oarg1, oarg2, oarg3, oargs) = default_obj;
    }

    // special handling for generators:
    // the call to function containing a yield should just create a new generator object.
    Box* res;
    if (func->isGenerator) {
        res = createGenerator(func, oarg1, oarg2, oarg3, oargs);
    } else {
        res = callCLFunc(f, rewrite_args, num_output_args, closure, NULL, oarg1, oarg2, oarg3, oargs);
    }

    return res;
}

Box* callCLFunc(CLFunction* f, CallRewriteArgs* rewrite_args, int num_output_args, BoxedClosure* closure,
                BoxedGenerator* generator, Box* oarg1, Box* oarg2, Box* oarg3, Box** oargs) {
    CompiledFunction* chosen_cf = pickVersion(f, num_output_args, oarg1, oarg2, oarg3, oargs);

    assert(chosen_cf->is_interpreted == (chosen_cf->code == NULL));
    if (chosen_cf->is_interpreted) {
        return interpretFunction(chosen_cf->func, num_output_args, closure, generator, oarg1, oarg2, oarg3, oargs);
    }

    if (rewrite_args) {
        rewrite_args->rewriter->addDependenceOn(chosen_cf->dependent_callsites);

        std::vector<RewriterVarUsage> arg_vec;
        // TODO this kind of embedded reference needs to be tracked by the GC somehow?
        // Or maybe it's ok, since we've guarded on the function object?
        if (closure)
            arg_vec.push_back(std::move(rewrite_args->rewriter->loadConst((intptr_t)closure, Location::forArg(0))));
        if (num_output_args >= 1)
            arg_vec.push_back(std::move(rewrite_args->arg1));
        if (num_output_args >= 2)
            arg_vec.push_back(std::move(rewrite_args->arg2));
        if (num_output_args >= 3)
            arg_vec.push_back(std::move(rewrite_args->arg3));
        if (num_output_args >= 4)
            arg_vec.push_back(std::move(rewrite_args->args));

        rewrite_args->out_rtn = rewrite_args->rewriter->call(true, (void*)chosen_cf->call, std::move(arg_vec));
        rewrite_args->out_success = true;
    }

    if (closure && generator)
        return chosen_cf->closure_generator_call(closure, generator, oarg1, oarg2, oarg3, oargs);
    else if (closure)
        return chosen_cf->closure_call(closure, oarg1, oarg2, oarg3, oargs);
    else if (generator)
        return chosen_cf->generator_call(generator, oarg1, oarg2, oarg3, oargs);
    else
        return chosen_cf->call(oarg1, oarg2, oarg3, oargs);
}


Box* runtimeCallInternal(Box* obj, CallRewriteArgs* rewrite_args, ArgPassSpec argspec, Box* arg1, Box* arg2, Box* arg3,
                         Box** args, const std::vector<const std::string*>* keyword_names) {
    int npassed_args = argspec.totalPassed();

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
                RewriterVarUsage v = rewrite_args->args.getAttr((i - 3) * sizeof(Box*),
                                                                RewriterVarUsage::KillFlag::NoKill, Location::any());
                v.addAttrGuard(BOX_CLS_OFFSET, (intptr_t)args[i - 3]->cls);
                v.setDoneUsing();
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
        if (callable == NULL) {
            callable = callFunc;
        }
        Box* res = callable(f, rewrite_args, argspec, arg1, arg2, arg3, args, keyword_names);
        return res;
    } else if (obj->cls == instancemethod_cls) {
        // TODO it's dumb but I should implement patchpoints here as well
        // duplicated with callattr
        BoxedInstanceMethod* im = static_cast<BoxedInstanceMethod*>(obj);

        if (rewrite_args && !rewrite_args->func_guarded) {
            rewrite_args->obj.addAttrGuard(INSTANCEMETHOD_FUNC_OFFSET, (intptr_t)im->func);
        }

        // Guard on which type of instancemethod (bound or unbound)
        // That is, if im->obj is NULL, guard on it being NULL
        // otherwise, guard on it being non-NULL
        if (rewrite_args) {
            rewrite_args->obj.addAttrGuard(INSTANCEMETHOD_OBJ_OFFSET, 0, im->obj != NULL);
        }

        // TODO guard on im->obj being NULL or not
        if (im->obj == NULL) {
            Box* f = im->func;
            if (rewrite_args) {
                rewrite_args->func_guarded = true;
                rewrite_args->args_guarded = true;
                rewrite_args->obj
                    = rewrite_args->obj.getAttr(INSTANCEMETHOD_FUNC_OFFSET, RewriterVarUsage::Kill, Location::any());
            }
            Box* res = runtimeCallInternal(f, rewrite_args, argspec, arg1, arg2, arg3, args, keyword_names);
            return res;
        }

        if (npassed_args <= 2) {
            Box* rtn;
            if (rewrite_args) {
                // Kind of weird that we don't need to give this a valid RewriterVar, but it shouldn't need to access it
                // (since we've already guarded on the function).
                // rewriter enforce that we give it one, though
                CallRewriteArgs srewrite_args(rewrite_args->rewriter, rewrite_args->obj.addUse(),
                                              rewrite_args->destination, rewrite_args->call_done_guarding);

                srewrite_args.arg1 = rewrite_args->obj.getAttr(INSTANCEMETHOD_OBJ_OFFSET,
                                                               RewriterVarUsage::KillFlag::Kill, Location::any());
                srewrite_args.func_guarded = true;
                srewrite_args.args_guarded = true;
                if (npassed_args >= 1)
                    srewrite_args.arg2 = std::move(rewrite_args->arg1);
                if (npassed_args >= 2)
                    srewrite_args.arg3 = std::move(rewrite_args->arg2);

                rtn = runtimeCallInternal(
                    im->func, &srewrite_args,
                    ArgPassSpec(argspec.num_args + 1, argspec.num_keywords, argspec.has_starargs, argspec.has_kwargs),
                    im->obj, arg1, arg2, NULL, keyword_names);

                if (!srewrite_args.out_success) {
                    rewrite_args = NULL;
                } else {
                    rewrite_args->out_rtn = std::move(srewrite_args.out_rtn);
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
        __builtin_extract_return_addr(__builtin_return_address(0)), num_orig_args, "runtimeCall"));
    Box* rtn;

    if (rewriter.get()) {
        // TODO feel weird about doing this; it either isn't necessary
        // or this kind of thing is necessary in a lot more places
        // rewriter->getArg(1).addGuard(npassed_args);

        CallRewriteArgs rewrite_args(rewriter.get(), std::move(rewriter->getArg(0)), rewriter->getReturnDestination(),
                                     true);
        if (npassed_args >= 1)
            rewrite_args.arg1 = std::move(rewriter->getArg(2));
        if (npassed_args >= 2)
            rewrite_args.arg2 = std::move(rewriter->getArg(3));
        if (npassed_args >= 3)
            rewrite_args.arg3 = std::move(rewriter->getArg(4));
        if (npassed_args >= 4)
            rewrite_args.args = std::move(rewriter->getArg(5));
        rtn = runtimeCallInternal(obj, &rewrite_args, argspec, arg1, arg2, arg3, args, keyword_names);

        if (!rewrite_args.out_success) {
            rewrite_args.ensureAllDone();
            rewriter.reset(NULL);
        } else if (rtn) {
            rewriter->commitReturning(std::move(rewrite_args.out_rtn));
        }
    } else {
        rtn = runtimeCallInternal(obj, NULL, argspec, arg1, arg2, arg3, args, keyword_names);
    }
    assert(rtn);

    return rtn;
}

extern "C" Box* binopInternal(Box* lhs, Box* rhs, int op_type, bool inplace, BinopRewriteArgs* rewrite_args) {
    // TODO handle the case of the rhs being a subclass of the lhs
    // this could get really annoying because you can dynamically make one type a subclass
    // of the other!

    if (rewrite_args) {
        // TODO probably don't need to guard on the lhs_cls since it
        // will get checked no matter what, but the check that should be
        // removed is probably the later one.
        // ie we should have some way of specifying what we know about the values
        // of objects and their attributes, and the attributes' attributes.
        rewrite_args->lhs.addAttrGuard(BOX_CLS_OFFSET, (intptr_t)lhs->cls);
        rewrite_args->rhs.addAttrGuard(BOX_CLS_OFFSET, (intptr_t)rhs->cls);
    }

    Box* irtn = NULL;
    if (inplace) {
        std::string iop_name = getInplaceOpName(op_type);
        if (rewrite_args) {
            CallRewriteArgs srewrite_args(rewrite_args->rewriter, rewrite_args->lhs.addUse(), rewrite_args->destination,
                                          rewrite_args->call_done_guarding);
            srewrite_args.arg1 = rewrite_args->rhs.addUse();
            srewrite_args.args_guarded = true;
            irtn = callattrInternal1(lhs, &iop_name, CLASS_ONLY, &srewrite_args, ArgPassSpec(1), rhs);

            if (!srewrite_args.out_success) {
                rewrite_args = NULL;
            } else if (irtn) {
                if (irtn == NotImplemented)
                    srewrite_args.out_rtn.ensureDoneUsing();
                else
                    rewrite_args->out_rtn = std::move(srewrite_args.out_rtn);
            }
        } else {
            irtn = callattrInternal1(lhs, &iop_name, CLASS_ONLY, NULL, ArgPassSpec(1), rhs);
        }

        if (irtn) {
            if (irtn != NotImplemented) {
                if (rewrite_args) {
                    rewrite_args->lhs.setDoneUsing();
                    rewrite_args->rhs.setDoneUsing();
                    rewrite_args->out_success = true;
                }
                return irtn;
            }
        }
    }

    const std::string& op_name = getOpName(op_type);
    Box* lrtn;
    if (rewrite_args) {
        CallRewriteArgs srewrite_args(rewrite_args->rewriter, std::move(rewrite_args->lhs), rewrite_args->destination,
                                      rewrite_args->call_done_guarding);
        srewrite_args.arg1 = std::move(rewrite_args->rhs);
        lrtn = callattrInternal1(lhs, &op_name, CLASS_ONLY, &srewrite_args, ArgPassSpec(1), rhs);

        if (!srewrite_args.out_success)
            rewrite_args = NULL;
        else if (lrtn) {
            if (lrtn == NotImplemented)
                srewrite_args.out_rtn.ensureDoneUsing();
            else
                rewrite_args->out_rtn = std::move(srewrite_args.out_rtn);
        }
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
            Rewriter::createRewriter(__builtin_extract_return_addr(__builtin_return_address(0)), 3, "binop"));

    Box* rtn;
    if (rewriter.get()) {
        // rewriter->trap();
        BinopRewriteArgs rewrite_args(rewriter.get(), rewriter->getArg(0), rewriter->getArg(1),
                                      rewriter->getReturnDestination(), true);
        rtn = binopInternal(lhs, rhs, op_type, false, &rewrite_args);
        assert(rtn);
        if (!rewrite_args.out_success) {
            rewrite_args.ensureAllDone();
            rewriter.reset(NULL);
        } else
            rewriter->commitReturning(std::move(rewrite_args.out_rtn));
    } else {
        rtn = binopInternal(lhs, rhs, op_type, false, NULL);
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
            Rewriter::createRewriter(__builtin_extract_return_addr(__builtin_return_address(0)), 3, "binop"));

    Box* rtn;
    if (rewriter.get()) {
        BinopRewriteArgs rewrite_args(rewriter.get(), rewriter->getArg(0), rewriter->getArg(1),
                                      rewriter->getReturnDestination(), true);
        rtn = binopInternal(lhs, rhs, op_type, true, &rewrite_args);
        if (!rewrite_args.out_success) {
            rewrite_args.ensureAllDone();
            rewriter.reset(NULL);
        } else {
            rewriter->commitReturning(std::move(rewrite_args.out_rtn));
        }
    } else {
        rtn = binopInternal(lhs, rhs, op_type, true, NULL);
    }

    return rtn;
}

Box* compareInternal(Box* lhs, Box* rhs, int op_type, CompareRewriteArgs* rewrite_args) {
    if (op_type == AST_TYPE::Is || op_type == AST_TYPE::IsNot) {
        bool neg = (op_type == AST_TYPE::IsNot);

        if (rewrite_args) {
            rewrite_args->rewriter->setDoneGuarding();
            RewriterVarUsage cmpres = rewrite_args->lhs.cmp(neg ? AST_TYPE::NotEq : AST_TYPE::Eq,
                                                            std::move(rewrite_args->rhs), rewrite_args->destination);
            rewrite_args->lhs.setDoneUsing();
            rewrite_args->out_rtn = rewrite_args->rewriter->call(false, (void*)boxBool, std::move(cmpres));
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

            Box* getitem = typeLookup(rhs->cls, "__getitem__", NULL);
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
        CallRewriteArgs crewrite_args(rewrite_args->rewriter, std::move(rewrite_args->lhs), rewrite_args->destination,
                                      rewrite_args->call_done_guarding);
        crewrite_args.arg1 = std::move(rewrite_args->rhs);
        lrtn = callattrInternal1(lhs, &op_name, CLASS_ONLY, &crewrite_args, ArgPassSpec(1), rhs);

        if (!crewrite_args.out_success)
            rewrite_args = NULL;
        else if (lrtn)
            rewrite_args->out_rtn = std::move(crewrite_args.out_rtn);
    } else {
        lrtn = callattrInternal1(lhs, &op_name, CLASS_ONLY, NULL, ArgPassSpec(1), rhs);
    }

    if (lrtn) {
        if (lrtn != NotImplemented) {
            bool can_patchpoint = !isUserDefined(lhs->cls) && !isUserDefined(rhs->cls);
            if (rewrite_args) {
                if (can_patchpoint) {
                    rewrite_args->out_success = true;
                } else {
                    rewrite_args->out_rtn.ensureDoneUsing();
                }
            }
            return lrtn;
        }
    }

    // TODO patch these cases
    if (rewrite_args) {
        rewrite_args->out_rtn.ensureDoneUsing();
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

#ifndef NDEBUG
    if ((lhs->cls == int_cls || lhs->cls == float_cls || lhs->cls == long_cls)
        && (rhs->cls == int_cls || rhs->cls == float_cls || rhs->cls == long_cls)) {
        Py_FatalError("missing comparison between these classes");
    }
#endif

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
        Rewriter::createRewriter(__builtin_extract_return_addr(__builtin_return_address(0)), 3, "compare"));

    Box* rtn;
    if (rewriter.get()) {
        // rewriter->trap();
        CompareRewriteArgs rewrite_args(rewriter.get(), std::move(rewriter->getArg(0)), std::move(rewriter->getArg(1)),
                                        rewriter->getReturnDestination(), true);
        rtn = compareInternal(lhs, rhs, op_type, &rewrite_args);
        if (!rewrite_args.out_success) {
            rewrite_args.ensureAllDone();
            rewriter.reset(NULL);
        } else
            rewriter->commitReturning(std::move(rewrite_args.out_rtn));
    } else {
        rtn = compareInternal(lhs, rhs, op_type, NULL);
    }

    return rtn;
}

extern "C" Box* unaryop(Box* operand, int op_type) {
    static StatCounter slowpath_unaryop("slowpath_unaryop");
    slowpath_unaryop.log();

    const std::string& op_name = getOpName(op_type);

    Box* attr_func = getclsattr_internal(operand, op_name, NULL);

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
        Rewriter::createRewriter(__builtin_extract_return_addr(__builtin_return_address(0)), 2, "getitem"));

    Box* rtn;
    if (rewriter.get()) {
        CallRewriteArgs rewrite_args(rewriter.get(), std::move(rewriter->getArg(0)), rewriter->getReturnDestination(),
                                     true);
        rewrite_args.arg1 = std::move(rewriter->getArg(1));

        rtn = callattrInternal1(value, &str_getitem, CLASS_ONLY, &rewrite_args, ArgPassSpec(1), slice);

        if (!rewrite_args.out_success) {
            rewrite_args.ensureAllDone();
            rewriter.reset(NULL);
        } else if (rtn)
            rewriter->commitReturning(std::move(rewrite_args.out_rtn));
    } else {
        rtn = callattrInternal1(value, &str_getitem, CLASS_ONLY, NULL, ArgPassSpec(1), slice);
    }

    if (rtn == NULL) {
        // different versions of python give different error messages for this:
        if (PYTHON_VERSION_MAJOR == 2 && PYTHON_VERSION_MINOR < 7) {
            raiseExcHelper(TypeError, "'%s' object is unsubscriptable", getTypeName(value)->c_str()); // tested on 2.6.6
        } else if (PYTHON_VERSION_MAJOR == 2 && PYTHON_VERSION_MINOR == 7 && PYTHON_VERSION_MICRO < 3) {
            raiseExcHelper(TypeError, "'%s' object is not subscriptable",
                           getTypeName(value)->c_str()); // tested on 2.7.1
        } else {
            // Changed to this in 2.7.3:
            raiseExcHelper(TypeError, "'%s' object has no attribute '__getitem__'",
                           getTypeName(value)->c_str()); // tested on 2.7.3
        }
    }

    return rtn;
}

// target[slice] = value
extern "C" void setitem(Box* target, Box* slice, Box* value) {
    static StatCounter slowpath_setitem("slowpath_setitem");
    slowpath_setitem.log();
    static std::string str_setitem("__setitem__");

    std::unique_ptr<Rewriter> rewriter(
        Rewriter::createRewriter(__builtin_extract_return_addr(__builtin_return_address(0)), 3, "setitem"));

    Box* rtn;
    if (rewriter.get()) {
        CallRewriteArgs rewrite_args(rewriter.get(), std::move(rewriter->getArg(0)), rewriter->getReturnDestination(),
                                     true);
        rewrite_args.arg1 = std::move(rewriter->getArg(1));
        rewrite_args.arg2 = std::move(rewriter->getArg(2));

        rtn = callattrInternal2(target, &str_setitem, CLASS_ONLY, &rewrite_args, ArgPassSpec(2), slice, value);

        if (!rewrite_args.out_success) {
            rewrite_args.ensureAllDone();
            rewriter.reset(NULL);
        } else if (rtn)
            rewrite_args.out_rtn.setDoneUsing();
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
        Rewriter::createRewriter(__builtin_extract_return_addr(__builtin_return_address(0)), 2, "delitem"));

    Box* rtn;
    if (rewriter.get()) {
        CallRewriteArgs rewrite_args(rewriter.get(), rewriter->getArg(0), rewriter->getReturnDestination(), true);
        rewrite_args.arg1 = std::move(rewriter->getArg(1));

        rtn = callattrInternal1(target, &str_delitem, CLASS_ONLY, &rewrite_args, ArgPassSpec(1), slice);

        if (!rewrite_args.out_success) {
            rewrite_args.ensureAllDone();
            rewriter.reset(NULL);
        } else if (rtn != NULL) {
            rewrite_args.out_rtn.setDoneUsing();
        }

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

void Box::delattr(const std::string& attr, DelattrRewriteArgs* rewrite_args) {
    // as soon as the hcls changes, the guard on hidden class won't pass.
    HCAttrs* attrs = getAttrsPtr();
    HiddenClass* hcls = attrs->hcls;
    HiddenClass* new_hcls = hcls->delAttrToMakeHC(attr);

    // The order of attributes is pertained as delAttrToMakeHC constructs
    // the new HiddenClass by invoking getOrMakeChild in the prevous order
    // of remaining attributes
    int num_attrs = hcls->attr_offsets.size();
    int offset = hcls->getOffset(attr);
    assert(offset >= 0);
    Box** start = attrs->attr_list->attrs;
    memmove(start + offset, start + offset + 1, (num_attrs - offset - 1) * sizeof(Box*));

    attrs->hcls = new_hcls;

    // guarantee the size of the attr_list equals the number of attrs
    int new_size = sizeof(HCAttrs::AttrList) + sizeof(Box*) * (num_attrs - 1);
    attrs->attr_list = (HCAttrs::AttrList*)gc::gc_realloc(attrs->attr_list, new_size);
}

extern "C" void delattr_internal(Box* obj, const std::string& attr, bool allow_custom,
                                 DelattrRewriteArgs* rewrite_args) {
    static const std::string delattr_str("__delattr__");
    static const std::string delete_str("__delete__");

    // custom __delattr__
    if (allow_custom) {
        Box* delAttr = typeLookup(obj->cls, delattr_str, NULL);
        if (delAttr != NULL) {
            Box* boxstr = boxString(attr);
            Box* rtn = runtimeCall2(delAttr, ArgPassSpec(2), obj, boxstr);
            return;
        }
    }

    // first check whether the deleting attribute is a descriptor
    Box* clsAttr = typeLookup(obj->cls, attr, NULL);
    if (clsAttr != NULL) {
        Box* delAttr = typeLookup(static_cast<BoxedClass*>(clsAttr->cls), delete_str, NULL);

        if (delAttr != NULL) {
            Box* boxstr = boxString(attr);
            Box* rtn = runtimeCall2(delAttr, ArgPassSpec(2), clsAttr, obj);
            return;
        }
    }

    // check if the attribute is in the instance's __dict__
    Box* attrVal = obj->getattr(attr, NULL);
    if (attrVal != NULL) {
        obj->delattr(attr, NULL);
    } else {
        // the exception cpthon throws is different when the class contains the attribute
        if (clsAttr != NULL) {
            raiseExcHelper(AttributeError, "'%s' object attribute '%s' is read-only", getTypeName(obj)->c_str(),
                           attr.c_str());
        } else {
            raiseAttributeError(obj, attr.c_str());
        }
    }
}

// del target.attr
extern "C" void delattr(Box* obj, const char* attr) {
    static StatCounter slowpath_delattr("slowpath_delattr");
    slowpath_delattr.log();

    if (obj->cls == type_cls) {
        BoxedClass* cobj = static_cast<BoxedClass*>(obj);
        if (!isUserDefined(cobj)) {
            raiseExcHelper(TypeError, "can't set attributes of built-in/extension type '%s'\n",
                           getNameOfClass(cobj)->c_str());
        }
    }


    delattr_internal(obj, attr, true, NULL);
}

extern "C" Box* getiter(Box* o) {
    // TODO add rewriting to this?  probably want to try to avoid this path though
    static const std::string iter_str("__iter__");
    Box* r = callattrInternal0(o, &iter_str, LookupScope::CLASS_ONLY, NULL, ArgPassSpec(0));
    if (r)
        return r;

    static const std::string getitem_str("__getitem__");
    if (typeLookup(o->cls, getitem_str, NULL)) {
        return new BoxedSeqIter(o);
    }

    raiseExcHelper(TypeError, "'%s' object is not iterable", getTypeName(o)->c_str());
}

llvm::iterator_range<BoxIterator> Box::pyElements() {
    Box* iter = getiter(this);
    assert(iter);
    return llvm::iterator_range<BoxIterator>(++BoxIterator(iter), BoxIterator(nullptr));
}

// For use on __init__ return values
static void assertInitNone(Box* obj) {
    if (obj != None) {
        raiseExcHelper(TypeError, "__init__() should return None, not '%s'", getTypeName(obj)->c_str());
    }
}

Box* typeNew(Box* _cls, Box* arg1, Box* arg2, Box** _args) {
    Box* arg3 = _args[0];

    if (!isSubclass(_cls->cls, type_cls))
        raiseExcHelper(TypeError, "type.__new__(X): X is not a type object (%s)", getTypeName(_cls)->c_str());

    BoxedClass* cls = static_cast<BoxedClass*>(_cls);
    if (!isSubclass(cls, type_cls))
        raiseExcHelper(TypeError, "type.__new__(%s): %s is not a subtype of type", getNameOfClass(cls)->c_str(),
                       getNameOfClass(cls)->c_str());

    if (arg2 == NULL) {
        assert(arg3 == NULL);
        BoxedClass* rtn = arg1->cls;
        return rtn;
    }

    RELEASE_ASSERT(arg3->cls == dict_cls, "%s", getTypeName(arg3)->c_str());
    BoxedDict* attr_dict = static_cast<BoxedDict*>(arg3);

    RELEASE_ASSERT(arg2->cls == tuple_cls, "");
    BoxedTuple* bases = static_cast<BoxedTuple*>(arg2);

    RELEASE_ASSERT(arg1->cls == str_cls, "");
    BoxedString* name = static_cast<BoxedString*>(arg1);

    BoxedClass* base;
    if (bases->elts.size() == 0) {
        bases = new BoxedTuple({ object_cls });
    }

    RELEASE_ASSERT(bases->elts.size() == 1, "");
    Box* _base = bases->elts[0];
    RELEASE_ASSERT(_base->cls == type_cls, "");
    base = static_cast<BoxedClass*>(_base);



    BoxedClass* made;

    if (base->instancesHaveAttrs()) {
        made = new BoxedClass(cls, base, NULL, base->attrs_offset, base->tp_basicsize, true);
    } else {
        assert(base->tp_basicsize % sizeof(void*) == 0);
        made = new BoxedClass(cls, base, NULL, base->tp_basicsize, base->tp_basicsize + sizeof(HCAttrs), true);
    }

    made->giveAttr("__module__", boxString(getCurrentModule()->name()));
    made->giveAttr("__doc__", None);

    for (const auto& p : attr_dict->d) {
        assert(p.first->cls == str_cls);
        made->setattr(static_cast<BoxedString*>(p.first)->s, p.second, NULL);
    }

    // Note: make sure to do this after assigning the attrs, since it will overwrite any defined __name__
    made->setattr("__name__", name, NULL);

    // TODO this function (typeNew) should probably call PyType_Ready

    made->tp_alloc = reinterpret_cast<decltype(cls->tp_alloc)>(PyType_GenericAlloc);

    return made;
}

Box* typeCallInternal(BoxedFunction* f, CallRewriteArgs* rewrite_args, ArgPassSpec argspec, Box* arg1, Box* arg2,
                      Box* arg3, Box** args, const std::vector<const std::string*>* keyword_names) {
    int npassed_args = argspec.totalPassed();

    static StatCounter slowpath_typecall("slowpath_typecall");
    slowpath_typecall.log();

    // TODO shouldn't have to redo this argument handling here...
    if (argspec.has_starargs) {
        rewrite_args = NULL;

        assert(argspec.num_args == 0); // doesn't need to be true, but assumed here
        Box* starargs = arg1;
        assert(starargs->cls == tuple_cls);
        BoxedTuple* targs = static_cast<BoxedTuple*>(starargs);

        int n = targs->elts.size();
        if (n >= 1)
            arg1 = targs->elts[0];
        if (n >= 2)
            arg2 = targs->elts[1];
        if (n >= 3)
            arg3 = targs->elts[2];
        if (n >= 4)
            args = &targs->elts[3];

        argspec = ArgPassSpec(n);
    }

    Box* _cls = arg1;

    RewriterVarUsage r_ccls(RewriterVarUsage::empty());
    RewriterVarUsage r_new(RewriterVarUsage::empty());
    RewriterVarUsage r_init(RewriterVarUsage::empty());
    Box* new_attr, *init_attr;
    if (rewrite_args) {
        assert(!argspec.has_starargs);
        assert(argspec.num_args > 0);

        rewrite_args->obj.setDoneUsing();
        // rewrite_args->rewriter->annotate(0);
        // rewrite_args->rewriter->trap();
        r_ccls = std::move(rewrite_args->arg1);
        // This is probably a duplicate, but it's hard to really convince myself of that.
        // Need to create a clear contract of who guards on what
        r_ccls.addGuard((intptr_t)arg1);
    }

    if (!isSubclass(_cls->cls, type_cls)) {
        raiseExcHelper(TypeError, "descriptor '__call__' requires a 'type' object but received an '%s'",
                       getTypeName(_cls)->c_str());
    }

    BoxedClass* cls = static_cast<BoxedClass*>(_cls);

    if (rewrite_args) {
        GetattrRewriteArgs grewrite_args(rewrite_args->rewriter, r_ccls.addUse(), rewrite_args->destination, false);
        new_attr = typeLookup(cls, _new_str, &grewrite_args);

        if (!grewrite_args.out_success)
            rewrite_args = NULL;
        else {
            if (new_attr) {
                r_new = std::move(grewrite_args.out_rtn);
                r_new.addGuard((intptr_t)new_attr);
            }
        }
    } else {
        new_attr = typeLookup(cls, _new_str, NULL);
    }
    assert(new_attr && "This should always resolve");

    if (rewrite_args) {
        GetattrRewriteArgs grewrite_args(rewrite_args->rewriter, r_ccls.addUse(), rewrite_args->destination, false);
        init_attr = typeLookup(cls, _init_str, &grewrite_args);

        if (!grewrite_args.out_success)
            rewrite_args = NULL;
        else {
            if (init_attr) {
                r_init = std::move(grewrite_args.out_rtn);
                r_init.addGuard((intptr_t)init_attr);
            }
            rewrite_args->rewriter->setDoneGuarding();
        }
    } else {
        init_attr = typeLookup(cls, _init_str, NULL);
    }
    // The init_attr should always resolve as well, but doesn't yet

    Box* made;
    RewriterVarUsage r_made(RewriterVarUsage::empty());

    ArgPassSpec new_argspec = argspec;
    if (npassed_args > 1 && new_attr == typeLookup(object_cls, _new_str, NULL)) {
        if (init_attr == typeLookup(object_cls, _init_str, NULL)) {
            raiseExcHelper(TypeError, objectNewParameterTypeErrorMsg());
        } else {
            new_argspec = ArgPassSpec(1);
        }
    }

    if (rewrite_args) {
        CallRewriteArgs srewrite_args(rewrite_args->rewriter, std::move(r_new), rewrite_args->destination,
                                      rewrite_args->call_done_guarding);
        int new_npassed_args = new_argspec.totalPassed();

        if (new_npassed_args >= 1)
            srewrite_args.arg1 = std::move(r_ccls);
        if (new_npassed_args >= 2)
            srewrite_args.arg2 = rewrite_args->arg2.addUse();
        if (new_npassed_args >= 3)
            srewrite_args.arg3 = rewrite_args->arg3.addUse();
        if (new_npassed_args >= 4)
            srewrite_args.args = rewrite_args->args.addUse();
        srewrite_args.args_guarded = true;
        srewrite_args.func_guarded = true;

        made = runtimeCallInternal(new_attr, &srewrite_args, new_argspec, cls, arg2, arg3, args, keyword_names);

        if (!srewrite_args.out_success) {
            rewrite_args = NULL;
        } else {
            r_made = std::move(srewrite_args.out_rtn);
        }
    } else {
        made = runtimeCallInternal(new_attr, NULL, new_argspec, cls, arg2, arg3, args, keyword_names);
    }

    assert(made);

    // Special-case (also a special case in CPython): if we just called type.__new__(arg), don't call __init__
    if (cls == type_cls && argspec == ArgPassSpec(2))
        return made;

    // If this is true, not supposed to call __init__:
    RELEASE_ASSERT(made->cls == cls, "allowed but unsupported (%s vs %s)", getNameOfClass(made->cls)->c_str(),
                   getNameOfClass(cls)->c_str());

    if (init_attr && init_attr != typeLookup(object_cls, _init_str, NULL)) {
        Box* initrtn;
        if (rewrite_args) {
            CallRewriteArgs srewrite_args(rewrite_args->rewriter, std::move(r_init), rewrite_args->destination,
                                          rewrite_args->call_done_guarding);
            if (npassed_args >= 1)
                srewrite_args.arg1 = r_made.addUse();
            if (npassed_args >= 2)
                srewrite_args.arg2 = std::move(rewrite_args->arg2);
            if (npassed_args >= 3)
                srewrite_args.arg3 = std::move(rewrite_args->arg3);
            if (npassed_args >= 4)
                srewrite_args.args = std::move(rewrite_args->args);
            srewrite_args.args_guarded = true;
            srewrite_args.func_guarded = true;

            // initrtn = callattrInternal(cls, &_init_str, INST_ONLY, &srewrite_args, argspec, made, arg2, arg3, args,
            // keyword_names);
            initrtn = runtimeCallInternal(init_attr, &srewrite_args, argspec, made, arg2, arg3, args, keyword_names);

            if (!srewrite_args.out_success) {
                rewrite_args = NULL;
            } else {
                rewrite_args->rewriter->call(false, (void*)assertInitNone, std::move(srewrite_args.out_rtn))
                    .setDoneUsing();
            }
        } else {
            // initrtn = callattrInternal(cls, &_init_str, INST_ONLY, NULL, argspec, made, arg2, arg3, args,
            // keyword_names);
            initrtn = runtimeCallInternal(init_attr, NULL, argspec, made, arg2, arg3, args, keyword_names);
        }
        assertInitNone(initrtn);
    } else {
        if (new_attr == NULL && npassed_args != 1) {
            // TODO not npassed args, since the starargs or kwargs could be null
            raiseExcHelper(TypeError, objectNewParameterTypeErrorMsg());
        }
    }

    if (rewrite_args) {
        rewrite_args->out_rtn = std::move(r_made);
        rewrite_args->out_success = true;
    }

    // Some of these might still be in use if rewrite_args was set to NULL
    r_init.ensureDoneUsing();
    r_ccls.ensureDoneUsing();
    r_made.ensureDoneUsing();
    r_init.ensureDoneUsing();
    if (rewrite_args) {
        rewrite_args->arg2.ensureDoneUsing();
        rewrite_args->arg3.ensureDoneUsing();
        rewrite_args->args.ensureDoneUsing();
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
        abort();
}

extern "C" void delGlobal(BoxedModule* m, std::string* name) {
    if (!m->getattr(*name)) {
        raiseExcHelper(NameError, "name '%s' is not defined", name->c_str());
    }
    m->delattr(*name, NULL);
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
            Rewriter::createRewriter(__builtin_extract_return_addr(__builtin_return_address(0)), 3, "getGlobal"));

        Box* r;
        if (rewriter.get()) {
            // rewriter->trap();

            GetattrRewriteArgs rewrite_args(rewriter.get(), rewriter->getArg(0), rewriter->getReturnDestination(),
                                            false);
            r = m->getattr(*name, &rewrite_args);
            if (!rewrite_args.obj.isDoneUsing()) {
                rewrite_args.obj.setDoneUsing();
            }
            if (!rewrite_args.out_success) {
                rewrite_args.ensureAllDone();
                rewriter.reset(NULL);
            }
            if (r) {
                if (rewriter.get()) {
                    rewriter->setDoneGuarding();
                    rewriter->commitReturning(std::move(rewrite_args.out_rtn));
                } else {
                    rewrite_args.out_rtn.setDoneUsing();
                }
                return r;
            }
        } else {
            r = m->getattr(*name, NULL);
            nopatch_getglobal.log();
            if (r) {
                return r;
            }
        }

        static StatCounter stat_builtins("getglobal_builtins");
        stat_builtins.log();

        if ((*name) == "__builtins__") {
            if (rewriter.get()) {
                RewriterVarUsage r_rtn
                    = rewriter->loadConst((intptr_t)builtins_module, rewriter->getReturnDestination());
                rewriter->setDoneGuarding();
                rewriter->commitReturning(std::move(r_rtn));
            }
            return builtins_module;
        }

        Box* rtn;
        if (rewriter.get()) {
            RewriterVarUsage builtins = rewriter->loadConst((intptr_t)builtins_module, Location::any());
            GetattrRewriteArgs rewrite_args(rewriter.get(), std::move(builtins), rewriter->getReturnDestination(),
                                            true);
            rtn = builtins_module->getattr(*name, &rewrite_args);

            if (!rtn || !rewrite_args.out_success) {
                rewrite_args.ensureAllDone();
                rewriter.reset(NULL);
            }

            if (rewriter.get()) {
                rewriter->commitReturning(std::move(rewrite_args.out_rtn));
            }
        } else {
            rtn = builtins_module->getattr(*name, NULL);
        }

        if (rtn)
            return rtn;
    }

    raiseExcHelper(NameError, "global name '%s' is not defined", name->c_str());
}

extern "C" Box* importFrom(Box* _m, const std::string* name) {
    assert(_m->cls == module_cls);

    BoxedModule* m = static_cast<BoxedModule*>(_m);

    Box* r = m->getattr(*name, NULL);
    if (r)
        return r;

    raiseExcHelper(ImportError, "cannot import name %s", name->c_str());
}

extern "C" void importStar(Box* _from_module, BoxedModule* to_module) {
    assert(_from_module->cls == module_cls);
    BoxedModule* from_module = static_cast<BoxedModule*>(_from_module);

    static std::string all_str("__all__");
    static std::string getitem_str("__getitem__");
    Box* all = from_module->getattr(all_str);

    if (all) {
        Box* all_getitem = typeLookup(all->cls, getitem_str, NULL);
        if (!all_getitem)
            raiseExcHelper(TypeError, "'%s' object does not support indexing", getTypeName(all)->c_str());

        int idx = 0;
        while (true) {
            Box* attr_name;
            try {
                attr_name = runtimeCallInternal2(all_getitem, NULL, ArgPassSpec(2), all, boxInt(idx));
            } catch (Box* b) {
                if (b->cls == IndexError)
                    break;
                throw;
            }
            idx++;

            if (attr_name->cls != str_cls)
                raiseExcHelper(TypeError, "attribute name must be string, not '%s'", getTypeName(attr_name)->c_str());

            BoxedString* casted_attr_name = static_cast<BoxedString*>(attr_name);
            Box* attr_value = from_module->getattr(casted_attr_name->s);

            if (!attr_value)
                raiseExcHelper(AttributeError, "'module' object has no attribute '%s'", casted_attr_name->s.c_str());

            to_module->setattr(casted_attr_name->s, attr_value, NULL);
        }
        return;
    }

    HCAttrs* module_attrs = from_module->getAttrsPtr();
    for (auto& p : module_attrs->hcls->attr_offsets) {
        if (p.first[0] == '_')
            continue;

        to_module->setattr(p.first, module_attrs->attr_list->attrs[p.second], NULL);
    }
}
}
