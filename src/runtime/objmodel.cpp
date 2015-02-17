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

#include "runtime/objmodel.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <sstream>
#include <stdint.h>

#include "asm_writing/icinfo.h"
#include "asm_writing/rewriter.h"
#include "capi/typeobject.h"
#include "capi/types.h"
#include "codegen/ast_interpreter.h"
#include "codegen/codegen.h"
#include "codegen/compvars.h"
#include "codegen/irgen/hooks.h"
#include "codegen/parser.h"
#include "codegen/type_recording.h"
#include "codegen/unwinding.h"
#include "core/ast.h"
#include "core/options.h"
#include "core/stats.h"
#include "core/types.h"
#include "gc/collector.h"
#include "gc/heap.h"
#include "runtime/capi.h"
#include "runtime/classobj.h"
#include "runtime/file.h"
#include "runtime/float.h"
#include "runtime/generator.h"
#include "runtime/ics.h"
#include "runtime/iterobject.h"
#include "runtime/long.h"
#include "runtime/rewrite_args.h"
#include "runtime/types.h"
#include "runtime/util.h"

#define BOX_CLS_OFFSET ((char*)&(((Box*)0x01)->cls) - (char*)0x1)
#define HCATTRS_HCLS_OFFSET ((char*)&(((HCAttrs*)0x01)->hcls) - (char*)0x1)
#define HCATTRS_ATTRS_OFFSET ((char*)&(((HCAttrs*)0x01)->attr_list) - (char*)0x1)
#define ATTRLIST_ATTRS_OFFSET ((char*)&(((HCAttrs::AttrList*)0x01)->attrs) - (char*)0x1)
#define ATTRLIST_KIND_OFFSET ((char*)&(((HCAttrs::AttrList*)0x01)->gc_header.kind_id) - (char*)0x1)
#define INSTANCEMETHOD_FUNC_OFFSET ((char*)&(((BoxedInstanceMethod*)0x01)->func) - (char*)0x1)
#define INSTANCEMETHOD_OBJ_OFFSET ((char*)&(((BoxedInstanceMethod*)0x01)->obj) - (char*)0x1)
#define CLASSMETHOD_CALLABLE_OFFSET ((char*)&(((BoxedClassmethod*)0x01)->cm_callable) - (char*)0x1)
#define BOOL_B_OFFSET ((char*)&(((BoxedBool*)0x01)->n) - (char*)0x1)
#define INT_N_OFFSET ((char*)&(((BoxedInt*)0x01)->n) - (char*)0x1)

namespace pyston {

static const std::string all_str("__all__");
static const std::string attr_str("__len__");
static const std::string call_str("__call__");
static const std::string contains_str("__contains__");
static const std::string delattr_str("__delattr__");
static const std::string delete_str("__delete__");
static const std::string delitem_str("__delitem__");
static const std::string getattribute_str("__getattribute__");
static const std::string getattr_str("__getattr__");
static const std::string getitem_str("__getitem__");
static const std::string get_str("__get__");
static const std::string hasnext_str("__hasnext__");
static const std::string init_str("__init__");
static const std::string iter_str("__iter__");
static const std::string new_str("__new__");
static const std::string none_str("None");
static const std::string repr_str("__repr__");
static const std::string setitem_str("__setitem__");
static const std::string set_str("__set__");
static const std::string str_str("__str__");

#if 0
void REWRITE_ABORTED(const char* reason) {
}
#else
#define REWRITE_ABORTED(reason) ((void)(reason))
#endif

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
    return cmp == True;
}

bool PyLt::operator()(Box* lhs, Box* rhs) const {
    // TODO fix this
    Box* cmp = compareInternal(lhs, rhs, AST_TYPE::Lt, NULL);
    assert(cmp->cls == bool_cls);
    return cmp == True;
}

extern "C" Box* deopt(AST_expr* expr, Box* value) {
    static StatCounter num_deopt("num_deopt");
    num_deopt.log();

    auto locals = getLocals(false /* filter */);
    auto execution_point = getExecutionPoint();

    // Should we only do this selectively?
    execution_point.cf->speculationFailed();

    return astInterpretFrom(execution_point.cf, expr, execution_point.current_stmt, value, locals);
}

extern "C" bool softspace(Box* b, bool newval) {
    assert(b);

    if (isSubclass(b->cls, file_cls)) {
        int& ss = static_cast<BoxedFile*>(b)->f_softspace;
        int r = ss;
        ss = newval;
        assert(r == 0 || r == 1);
        return (bool)r;
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

bool isInstance(Box* obj, BoxedClass* cls) {
    return isSubclass(obj->cls, cls);
}

extern "C" bool isSubclass(BoxedClass* child, BoxedClass* parent) {
    // TODO the class is allowed to override this using __subclasscheck__
    while (child) {
        if (child == parent)
            return true;
        child = child->tp_base;
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
                       getNameOfClass(static_cast<BoxedClass*>(obj)), attr);
    } else {
        raiseAttributeErrorStr(getTypeName(obj), attr);
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

void BoxedClass::freeze() {
    assert(!is_constant);
    assert(tp_name); // otherwise debugging will be very hard

    fixup_slot_dispatchers(this);

    is_constant = true;
}

BoxedClass::BoxedClass(BoxedClass* base, gcvisit_func gc_visit, int attrs_offset, int instance_size,
                       bool is_user_defined)
    : BoxVar(0), gc_visit(gc_visit), simple_destructor(NULL), attrs_offset(attrs_offset), is_constant(false),
      is_user_defined(is_user_defined), is_pyston_class(true) {

    // Zero out the CPython tp_* slots:
    memset(&tp_name, 0, (char*)(&tp_version_tag + 1) - (char*)(&tp_name));
    tp_basicsize = instance_size;

    tp_flags |= Py_TPFLAGS_CHECKTYPES;
    tp_flags |= Py_TPFLAGS_BASETYPE;
    tp_flags |= Py_TPFLAGS_HAVE_CLASS;
    tp_flags |= Py_TPFLAGS_HAVE_GC;

    tp_base = base;

    if (tp_base) {
        assert(tp_base->tp_alloc);
        tp_alloc = tp_base->tp_alloc;
    } else {
        assert(object_cls == NULL);
        tp_alloc = PystonType_GenericAlloc;
    }

    if (cls == NULL) {
        assert(type_cls == NULL);
    } else {
        assert(isSubclass(cls, type_cls));
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

BoxedHeapClass::BoxedHeapClass(BoxedClass* base, gcvisit_func gc_visit, int attrs_offset, int instance_size,
                               bool is_user_defined, const std::string& name)
    : BoxedHeapClass(base, gc_visit, attrs_offset, instance_size, is_user_defined, new BoxedString(name)) {
}

BoxedHeapClass::BoxedHeapClass(BoxedClass* base, gcvisit_func gc_visit, int attrs_offset, int instance_size,
                               bool is_user_defined)
    : BoxedClass(base, gc_visit, attrs_offset, instance_size, is_user_defined), ht_name(NULL), ht_slots(NULL) {

    // This constructor is only used for bootstrapping purposes to be called for types that
    // are initialized before str_cls.
    // Therefore, we assert that str_cls is uninitialized to make sure this isn't called at
    // an inappropriate time.
    assert(str_cls == NULL);

    tp_as_number = &as_number;
    tp_as_mapping = &as_mapping;
    tp_as_sequence = &as_sequence;
    tp_as_buffer = &as_buffer;
    tp_flags |= Py_TPFLAGS_HEAPTYPE;

    memset(&as_number, 0, sizeof(as_number));
    memset(&as_mapping, 0, sizeof(as_mapping));
    memset(&as_sequence, 0, sizeof(as_sequence));
    memset(&as_buffer, 0, sizeof(as_buffer));
}

BoxedHeapClass::BoxedHeapClass(BoxedClass* base, gcvisit_func gc_visit, int attrs_offset, int instance_size,
                               bool is_user_defined, BoxedString* name)
    : BoxedClass(base, gc_visit, attrs_offset, instance_size, is_user_defined), ht_name(name), ht_slots(NULL) {

    tp_as_number = &as_number;
    tp_as_mapping = &as_mapping;
    tp_as_sequence = &as_sequence;
    tp_as_buffer = &as_buffer;
    tp_name = ht_name->s.c_str();
    tp_flags |= Py_TPFLAGS_HEAPTYPE;

    memset(&as_number, 0, sizeof(as_number));
    memset(&as_mapping, 0, sizeof(as_mapping));
    memset(&as_sequence, 0, sizeof(as_sequence));
    memset(&as_buffer, 0, sizeof(as_buffer));
}

std::string getFullNameOfClass(BoxedClass* cls) {
    Box* b = cls->getattr("__module__");
    if (!b)
        return cls->tp_name;
    assert(b);
    if (b->cls != str_cls)
        return cls->tp_name;

    BoxedString* module = static_cast<BoxedString*>(b);

    return module->s + "." + cls->tp_name;
}

std::string getFullTypeName(Box* o) {
    return getFullNameOfClass(o->cls);
}

const char* getTypeName(Box* b) {
    return b->cls->tp_name;
}

const char* getNameOfClass(BoxedClass* cls) {
    return cls->tp_name;
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

HCAttrs* Box::getHCAttrsPtr() {
    assert(cls->instancesHaveHCAttrs());

    char* p = reinterpret_cast<char*>(this);
    p += cls->attrs_offset;
    return reinterpret_cast<HCAttrs*>(p);
}

BoxedDict* Box::getDict() {
    assert(cls->instancesHaveDictAttrs());

    char* p = reinterpret_cast<char*>(this);
    p += cls->tp_dictoffset;

    BoxedDict** d_ptr = reinterpret_cast<BoxedDict**>(p);
    BoxedDict* d = *d_ptr;
    if (!d) {
        d = *d_ptr = new BoxedDict();
    }

    assert(d->cls == dict_cls);
    return d;
}

Box* Box::getattr(const std::string& attr, GetattrRewriteArgs* rewrite_args) {

    if (rewrite_args)
        rewrite_args->obj->addAttrGuard(BOX_CLS_OFFSET, (intptr_t)cls);

    // Have to guard on the memory layout of this object.
    // Right now, guard on the specific Python-class, which in turn
    // specifies the C structure.
    // In the future, we could create another field (the flavor?)
    // that also specifies the structure and can include multiple
    // classes.
    // Only matters if we end up getting multiple classes with the same
    // structure (ex user class) and the same hidden classes, because
    // otherwise the guard will fail anyway.;
    if (cls->instancesHaveHCAttrs()) {
        if (rewrite_args)
            rewrite_args->out_success = true;

        HCAttrs* attrs = getHCAttrsPtr();
        HiddenClass* hcls = attrs->hcls;

        if (rewrite_args) {
            if (!rewrite_args->obj_hcls_guarded)
                rewrite_args->obj->addAttrGuard(cls->attrs_offset + HCATTRS_HCLS_OFFSET, (intptr_t)hcls);
        }

        int offset = hcls->getOffset(attr);
        if (offset == -1) {
            return NULL;
        }

        if (rewrite_args) {
            // TODO using the output register as the temporary makes register allocation easier
            // since we don't need to clobber a register, but does it make the code slower?
            RewriterVar* r_attrs
                = rewrite_args->obj->getAttr(cls->attrs_offset + HCATTRS_ATTRS_OFFSET, Location::any());
            rewrite_args->out_rtn = r_attrs->getAttr(offset * sizeof(Box*) + ATTRLIST_ATTRS_OFFSET, Location::any());
        }

        Box* rtn = attrs->attr_list->attrs[offset];
        return rtn;
    }

    if (cls->instancesHaveDictAttrs()) {
        if (rewrite_args)
            REWRITE_ABORTED("");

        BoxedDict* d = getDict();

        Box* key = boxString(attr);
        auto it = d->d.find(key);
        if (it == d->d.end())
            return NULL;
        return it->second;
    }

    if (rewrite_args)
        rewrite_args->out_success = true;

    return NULL;
}

void Box::setattr(const std::string& attr, Box* val, SetattrRewriteArgs* rewrite_args) {
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
        rewrite_args->obj->addAttrGuard(BOX_CLS_OFFSET, (intptr_t)cls);

    RELEASE_ASSERT(attr != none_str || this == builtins_module, "can't assign to None");

    if (cls->instancesHaveHCAttrs()) {
        HCAttrs* attrs = getHCAttrsPtr();
        HiddenClass* hcls = attrs->hcls;
        int numattrs = hcls->attr_offsets.size();

        int offset = hcls->getOffset(attr);

        if (rewrite_args) {
            rewrite_args->obj->addAttrGuard(cls->attrs_offset + HCATTRS_HCLS_OFFSET, (intptr_t)hcls);
            // rewrite_args->rewriter->addDecision(offset == -1 ? 1 : 0);
        }

        if (offset >= 0) {
            assert(offset < numattrs);
            Box* prev = attrs->attr_list->attrs[offset];
            attrs->attr_list->attrs[offset] = val;

            if (rewrite_args) {

                RewriterVar* r_hattrs
                    = rewrite_args->obj->getAttr(cls->attrs_offset + HCATTRS_ATTRS_OFFSET, Location::any());

                r_hattrs->setAttr(offset * sizeof(Box*) + ATTRLIST_ATTRS_OFFSET, rewrite_args->attrval);

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

        RewriterVar* r_new_array2 = NULL;
        int new_size = sizeof(HCAttrs::AttrList) + sizeof(Box*) * (numattrs + 1);
        if (numattrs == 0) {
            attrs->attr_list = (HCAttrs::AttrList*)gc_alloc(new_size, gc::GCKind::UNTRACKED);
            if (rewrite_args) {
                RewriterVar* r_newsize = rewrite_args->rewriter->loadConst(new_size, Location::forArg(0));
                RewriterVar* r_kind
                    = rewrite_args->rewriter->loadConst((int)gc::GCKind::UNTRACKED, Location::forArg(1));
                r_new_array2 = rewrite_args->rewriter->call(false, (void*)gc::gc_alloc, r_newsize, r_kind);
            }
        } else {
            attrs->attr_list = (HCAttrs::AttrList*)gc::gc_realloc(attrs->attr_list, new_size);
            if (rewrite_args) {
                RewriterVar* r_oldarray
                    = rewrite_args->obj->getAttr(cls->attrs_offset + HCATTRS_ATTRS_OFFSET, Location::forArg(0));
                RewriterVar* r_newsize = rewrite_args->rewriter->loadConst(new_size, Location::forArg(1));
                r_new_array2 = rewrite_args->rewriter->call(false, (void*)gc::gc_realloc, r_oldarray, r_newsize);
            }
        }
        // Don't set the new hcls until after we do the allocation for the new attr_list;
        // that allocation can cause a collection, and we want the collector to always
        // see a consistent state between the hcls and the attr_list
        attrs->hcls = new_hcls;

        if (rewrite_args) {
            r_new_array2->setAttr(numattrs * sizeof(Box*) + ATTRLIST_ATTRS_OFFSET, rewrite_args->attrval);
            rewrite_args->obj->setAttr(cls->attrs_offset + HCATTRS_ATTRS_OFFSET, r_new_array2);

            RewriterVar* r_hcls = rewrite_args->rewriter->loadConst((intptr_t)new_hcls);
            rewrite_args->obj->setAttr(cls->attrs_offset + HCATTRS_HCLS_OFFSET, r_hcls);

            rewrite_args->out_success = true;
        }
        attrs->attr_list->attrs[numattrs] = val;
        return;
    }

    if (cls->instancesHaveDictAttrs()) {
        BoxedDict* d = getDict();
        d->d[boxString(attr)] = val;
        return;
    }

    // Unreachable
    abort();
}

Box* typeLookup(BoxedClass* cls, const std::string& attr, GetattrRewriteArgs* rewrite_args) {
    Box* val;

    if (rewrite_args) {
        assert(!rewrite_args->out_success);

        RewriterVar* obj_saved = rewrite_args->obj;

        val = cls->getattr(attr, rewrite_args);
        assert(rewrite_args->out_success);
        if (!val and cls->tp_base) {
            rewrite_args->out_success = false;
            rewrite_args->obj = obj_saved->getAttr(offsetof(BoxedClass, tp_base));
            val = typeLookup(cls->tp_base, attr, rewrite_args);
        }
        return val;
    } else {
        val = cls->getattr(attr, NULL);
        if (!val and cls->tp_base)
            return typeLookup(cls->tp_base, attr, NULL);
        return val;
    }
}

bool isNondataDescriptorInstanceSpecialCase(Box* descr) {
    return descr->cls == function_cls || descr->cls == instancemethod_cls || descr->cls == staticmethod_cls
           || descr->cls == classmethod_cls;
}

Box* nondataDescriptorInstanceSpecialCases(GetattrRewriteArgs* rewrite_args, Box* obj, Box* descr, RewriterVar* r_descr,
                                           bool for_call, Box** bind_obj_out, RewriterVar** r_bind_obj_out) {
    // Special case: non-data descriptor: function, instancemethod or classmethod
    // Returns a bound instancemethod
    if (descr->cls == function_cls || descr->cls == instancemethod_cls || descr->cls == classmethod_cls) {
        Box* im_self = NULL, * im_func = NULL;
        RewriterVar* r_im_self = NULL, * r_im_func = NULL;
        if (descr->cls == function_cls) {
            im_self = obj;
            im_func = descr;
            if (rewrite_args) {
                r_im_self = rewrite_args->obj;
                r_im_func = r_descr;
            }
        } else if (descr->cls == classmethod_cls) {
            static StatCounter slowpath("slowpath_classmethod_get");
            slowpath.log();

            BoxedClassmethod* cm = static_cast<BoxedClassmethod*>(descr);
            im_self = obj->cls;
            if (cm->cm_callable == NULL) {
                raiseExcHelper(RuntimeError, "uninitialized classmethod object");
            }
            im_func = cm->cm_callable;

            if (rewrite_args) {
                r_im_self = rewrite_args->obj->getAttr(BOX_CLS_OFFSET);
                r_im_func = r_descr->getAttr(CLASSMETHOD_CALLABLE_OFFSET);
                r_im_func->addGuardNotEq(0);
            }
        } else if (descr->cls == instancemethod_cls) {
            static StatCounter slowpath("slowpath_instancemethod_get");
            slowpath.log();

            BoxedInstanceMethod* im = static_cast<BoxedInstanceMethod*>(descr);
            if (im->obj != NULL) {
                if (rewrite_args) {
                    r_descr->addAttrGuard(offsetof(BoxedInstanceMethod, obj), 0, /* negate */ true);
                }
                return descr;
            } else {
                // TODO subclass check
                im_self = obj;
                im_func = im->func;
                if (rewrite_args) {
                    r_descr->addAttrGuard(offsetof(BoxedInstanceMethod, obj), 0, /* negate */ false);
                    r_im_self = rewrite_args->obj;
                    r_im_func = r_descr->getAttr(offsetof(BoxedInstanceMethod, func));
                }
            }
        } else {
            assert(false);
        }

        if (!for_call) {
            if (rewrite_args) {
                rewrite_args->out_rtn
                    = rewrite_args->rewriter->call(false, (void*)boxInstanceMethod, r_im_self, r_im_func);
                rewrite_args->out_success = true;
            }
            return boxInstanceMethod(im_self, im_func);
        } else {
            *bind_obj_out = im_self;
            if (rewrite_args) {
                rewrite_args->out_rtn = r_im_func;
                rewrite_args->out_success = true;
                *r_bind_obj_out = r_im_self;
            }
            return im_func;
        }
    }

    else if (descr->cls == staticmethod_cls) {
        static StatCounter slowpath("slowpath_staticmethod_get");
        slowpath.log();

        BoxedStaticmethod* sm = static_cast<BoxedStaticmethod*>(descr);
        if (sm->sm_callable == NULL) {
            raiseExcHelper(RuntimeError, "uninitialized staticmethod object");
        }

        if (rewrite_args) {
            RewriterVar* r_sm_callable = r_descr->getAttr(offsetof(BoxedStaticmethod, sm_callable));
            r_sm_callable->addGuardNotEq(0);
            rewrite_args->out_success = true;
            rewrite_args->out_rtn = r_sm_callable;
        }

        return sm->sm_callable;
    }

    return NULL;
}

Box* descriptorClsSpecialCases(GetattrRewriteArgs* rewrite_args, BoxedClass* cls, Box* descr, RewriterVar* r_descr,
                               bool for_call, Box** bind_obj_out, RewriterVar** r_bind_obj_out) {
    // Special case: functions
    if (descr->cls == function_cls || descr->cls == instancemethod_cls) {
        if (rewrite_args)
            r_descr->addAttrGuard(BOX_CLS_OFFSET, (uint64_t)descr->cls);

        if (!for_call && descr->cls == function_cls) {
            if (rewrite_args) {
                // return an unbound instancemethod
                rewrite_args->out_rtn = rewrite_args->rewriter->call(false, (void*)boxUnboundInstanceMethod, r_descr);
                rewrite_args->out_success = true;
            }
            return boxUnboundInstanceMethod(descr);
        }

        if (rewrite_args) {
            rewrite_args->out_success = true;
            rewrite_args->out_rtn = r_descr;
        }
        return descr;
    }

    // Special case: classmethod descriptor
    if (descr->cls == classmethod_cls) {
        if (rewrite_args)
            r_descr->addAttrGuard(BOX_CLS_OFFSET, (uint64_t)descr->cls);

        BoxedClassmethod* cm = static_cast<BoxedClassmethod*>(descr);
        Box* im_func = cm->cm_callable;

        RewriterVar* r_im_self = NULL;

        if (rewrite_args)
            r_im_self = rewrite_args->obj->getAttr(BOX_CLS_OFFSET);

        if (!for_call) {
            if (rewrite_args) {
                RewriterVar* r_im_func = r_descr->getAttr(CLASSMETHOD_CALLABLE_OFFSET);
                r_im_func->addGuardNotEq(0);

                rewrite_args->out_rtn
                    = rewrite_args->rewriter->call(false, (void*)boxInstanceMethod, r_im_self, r_im_func);
                rewrite_args->out_success = true;
            }
            return boxInstanceMethod(cls, im_func);
        }

        if (rewrite_args) {
            *r_bind_obj_out = r_im_self;
            rewrite_args->out_success = true;
            rewrite_args->out_rtn = r_descr;
        }
        *bind_obj_out = cls;
        return descr;
    }

    // Special case: member descriptor
    if (descr->cls == member_cls) {
        if (rewrite_args)
            r_descr->addAttrGuard(BOX_CLS_OFFSET, (uint64_t)descr->cls);

        if (rewrite_args) {
            // Actually just return val (it's a descriptor but only
            // has special behaviour for instance lookups - see below)
            rewrite_args->out_rtn = r_descr;
            rewrite_args->out_success = true;
        }
        return descr;
    }

    return NULL;
}

Box* boxChar(char c) {
    char d[1];
    d[0] = c;
    return new BoxedString(std::string(d, 1));
}

static Box* noneIfNull(Box* b) {
    if (b == NULL) {
        return None;
    } else {
        return b;
    }
}

static Box* boxStringOrNone(const char* s) {
    if (s == NULL) {
        return None;
    } else {
        return boxString(std::string(s));
    }
}

static Box* boxStringFromCharPtr(const char* s) {
    return boxString(std::string(s));
}

Box* dataDescriptorInstanceSpecialCases(GetattrRewriteArgs* rewrite_args, const std::string& attr_name, Box* obj,
                                        Box* descr, RewriterVar* r_descr, bool for_call, Box** bind_obj_out,
                                        RewriterVar** r_bind_obj_out) {
    // Special case: data descriptor: member descriptor
    if (descr->cls == member_cls) {
        static StatCounter slowpath("slowpath_member_descriptor_get");
        slowpath.log();

        BoxedMemberDescriptor* member_desc = static_cast<BoxedMemberDescriptor*>(descr);
        // TODO should also have logic to raise a type error if type of obj is wrong

        if (rewrite_args) {
            // TODO we could use offset as the index in the assembly lookup rather than hardcoding
            // the value in the assembly and guarding on it be the same.
            r_descr->addAttrGuard(offsetof(BoxedMemberDescriptor, offset), member_desc->offset);

            // This could be optimized if addAttrGuard supported things < 64 bits
            static_assert(sizeof(member_desc->type) == 4, "assumed by assembly instruction below");
            r_descr->getAttr(offsetof(BoxedMemberDescriptor, type), Location::any(), assembler::MovType::ZLQ)
                ->addGuard(member_desc->type);
        }

        switch (member_desc->type) {
            case BoxedMemberDescriptor::OBJECT_EX: {
                if (rewrite_args) {
                    rewrite_args->out_rtn = rewrite_args->obj->getAttr(member_desc->offset, rewrite_args->destination);
                    rewrite_args->out_rtn->addGuardNotEq(0);
                    rewrite_args->out_success = true;
                }

                Box* rtn = *reinterpret_cast<Box**>((char*)obj + member_desc->offset);
                if (rtn == NULL) {
                    raiseExcHelper(AttributeError, "%s", attr_name.c_str());
                }
                return rtn;
            }
            case BoxedMemberDescriptor::OBJECT: {
                if (rewrite_args) {
                    RewriterVar* r_interm = rewrite_args->obj->getAttr(member_desc->offset, rewrite_args->destination);
                    // TODO would be faster to not use a call
                    rewrite_args->out_rtn = rewrite_args->rewriter->call(false, (void*)noneIfNull, r_interm);
                    rewrite_args->out_success = true;
                }

                Box* rtn = *reinterpret_cast<Box**>((char*)obj + member_desc->offset);
                return noneIfNull(rtn);
            }

            case BoxedMemberDescriptor::DOUBLE: {
                if (rewrite_args) {
                    RewriterVar* r_unboxed_val = rewrite_args->obj->getAttrDouble(member_desc->offset, assembler::XMM0);
                    std::vector<RewriterVar*> normal_args;
                    std::vector<RewriterVar*> float_args;
                    float_args.push_back(r_unboxed_val);
                    rewrite_args->out_rtn
                        = rewrite_args->rewriter->call(false, (void*)boxFloat, normal_args, float_args);
                    rewrite_args->out_success = true;
                }

                double rtn = *reinterpret_cast<double*>((char*)obj + member_desc->offset);
                return boxFloat(rtn);
            }
            case BoxedMemberDescriptor::FLOAT: {
                if (rewrite_args) {
                    RewriterVar* r_unboxed_val = rewrite_args->obj->getAttrFloat(member_desc->offset, assembler::XMM0);
                    std::vector<RewriterVar*> normal_args;
                    std::vector<RewriterVar*> float_args;
                    float_args.push_back(r_unboxed_val);
                    rewrite_args->out_rtn
                        = rewrite_args->rewriter->call(false, (void*)boxFloat, normal_args, float_args);
                    rewrite_args->out_success = true;
                }

                float rtn = *reinterpret_cast<float*>((char*)obj + member_desc->offset);
                return boxFloat((double)rtn);
            }

#define CASE_INTEGER_TYPE(TYPE, type, boxFn, cast)                                                                     \
    case BoxedMemberDescriptor::TYPE: {                                                                                \
        if (rewrite_args) {                                                                                            \
            RewriterVar* r_unboxed_val = rewrite_args->obj->getAttrCast<type, cast>(member_desc->offset);              \
            rewrite_args->out_rtn = rewrite_args->rewriter->call(false, (void*)boxFn, r_unboxed_val);                  \
            rewrite_args->out_success = true;                                                                          \
        }                                                                                                              \
        type rtn = *reinterpret_cast<type*>((char*)obj + member_desc->offset);                                         \
        return boxFn((cast)rtn);                                                                                       \
    }
                // Note that (a bit confusingly) boxInt takes int64_t, not an int
                CASE_INTEGER_TYPE(BOOL, bool, boxBool, bool)
                CASE_INTEGER_TYPE(BYTE, int8_t, boxInt, int64_t)
                CASE_INTEGER_TYPE(INT, int, boxInt, int64_t)
                CASE_INTEGER_TYPE(SHORT, short, boxInt, int64_t)
                CASE_INTEGER_TYPE(LONG, long, boxInt, int64_t)
                CASE_INTEGER_TYPE(CHAR, char, boxChar, char)
                CASE_INTEGER_TYPE(UBYTE, uint8_t, PyLong_FromUnsignedLong, unsigned long)
                CASE_INTEGER_TYPE(USHORT, unsigned short, PyLong_FromUnsignedLong, unsigned long)
                CASE_INTEGER_TYPE(UINT, unsigned int, PyLong_FromUnsignedLong, unsigned long)
                CASE_INTEGER_TYPE(ULONG, unsigned long, PyLong_FromUnsignedLong, unsigned long)
                CASE_INTEGER_TYPE(LONGLONG, long long, PyLong_FromLongLong, long long)
                CASE_INTEGER_TYPE(ULONGLONG, unsigned long long, PyLong_FromUnsignedLongLong, unsigned long long)
                CASE_INTEGER_TYPE(PYSSIZET, Py_ssize_t, boxInt, Py_ssize_t)

            case BoxedMemberDescriptor::STRING: {
                if (rewrite_args) {
                    RewriterVar* r_interm = rewrite_args->obj->getAttr(member_desc->offset, rewrite_args->destination);
                    rewrite_args->out_rtn = rewrite_args->rewriter->call(false, (void*)boxStringOrNone, r_interm);
                    rewrite_args->out_success = true;
                }

                char* rtn = *reinterpret_cast<char**>((char*)obj + member_desc->offset);
                return boxStringOrNone(rtn);
            }
            case BoxedMemberDescriptor::STRING_INPLACE: {
                if (rewrite_args) {
                    rewrite_args->out_rtn = rewrite_args->rewriter->call(
                        false, (void*)boxStringFromCharPtr,
                        rewrite_args->rewriter->add(rewrite_args->obj, member_desc->offset, rewrite_args->destination));
                    rewrite_args->out_success = true;
                }

                rewrite_args = NULL;
                REWRITE_ABORTED("");
                char* rtn = reinterpret_cast<char*>((char*)obj + member_desc->offset);
                return boxString(std::string(rtn));
            }

            default:
                RELEASE_ASSERT(0, "%d", member_desc->type);
        }
    }

    else if (descr->cls == property_cls) {
        rewrite_args = NULL; // TODO
        REWRITE_ABORTED("");

        BoxedProperty* prop = static_cast<BoxedProperty*>(descr);
        if (prop->prop_get == NULL || prop->prop_get == None) {
            raiseExcHelper(AttributeError, "unreadable attribute");
        }
        return runtimeCallInternal1(prop->prop_get, NULL, ArgPassSpec(1), obj);
    }

    // Special case: data descriptor: getset descriptor
    else if (descr->cls == getset_cls) {
        BoxedGetsetDescriptor* getset_descr = static_cast<BoxedGetsetDescriptor*>(descr);

        // TODO some more checks should go here
        // getset descriptors (and some other types of builtin descriptors I think) should have
        // a field which gives the type that the descriptor should apply to. We need to check that obj
        // is of that type.

        if (getset_descr->get == NULL) {
            raiseExcHelper(AttributeError, "attribute '%s' of '%s' object is not readable", attr_name.c_str(),
                           getTypeName(getset_descr));
        }

        // Abort because right now we can't call twice in a rewrite
        if (for_call) {
            rewrite_args = NULL;
        }

        if (rewrite_args) {
            // hmm, maybe we should write assembly which can look up the function address and call any function
            r_descr->addAttrGuard(offsetof(BoxedGetsetDescriptor, get), (intptr_t)getset_descr->get);

            RewriterVar* r_closure = r_descr->getAttr(offsetof(BoxedGetsetDescriptor, closure));
            rewrite_args->out_rtn = rewrite_args->rewriter->call(
                /* can_call_into_python */ true, (void*)getset_descr->get, rewrite_args->obj, r_closure);
            rewrite_args->out_success = true;
        }

        return getset_descr->get(obj, getset_descr->closure);
    }

    return NULL;
}

inline Box* getclsattr_internal(Box* obj, const std::string& attr, GetattrRewriteArgs* rewrite_args) {
    return getattrInternalGeneral(obj, attr, rewrite_args,
                                  /* cls_only */ true,
                                  /* for_call */ false, NULL, NULL);
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
        GetattrRewriteArgs rewrite_args(rewriter.get(), rewriter->getArg(0), rewriter->getReturnDestination());
        gotten = getclsattr_internal(obj, attr, &rewrite_args);

        if (rewrite_args.out_success && gotten) {
            rewriter->commitReturning(rewrite_args.out_rtn);
        }
#endif
}
else {
    gotten = getclsattr_internal(obj, attr, NULL);
}
RELEASE_ASSERT(gotten, "%s:%s", getTypeName(obj), attr);

return gotten;
}


// Does a simple call of the descriptor's __get__ if it exists;
// this function is useful for custom getattribute implementations that already know whether the descriptor
// came from the class or not.
Box* processDescriptorOrNull(Box* obj, Box* inst, Box* owner) {
    Box* descr_r
        = callattrInternal(obj, &get_str, LookupScope::CLASS_ONLY, NULL, ArgPassSpec(2), inst, owner, NULL, NULL, NULL);
    return descr_r;
}

Box* processDescriptor(Box* obj, Box* inst, Box* owner) {
    Box* descr_r = processDescriptorOrNull(obj, inst, owner);
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
                            bool for_call, Box** bind_obj_out, RewriterVar** r_bind_obj_out) {
    if (for_call) {
        *bind_obj_out = NULL;
    }

    if (obj->cls == closure_cls) {
        Box* val = NULL;
        if (rewrite_args) {
            GetattrRewriteArgs hrewrite_args(rewrite_args->rewriter, rewrite_args->obj, rewrite_args->destination);
            val = obj->getattr(attr, &hrewrite_args);

            if (!hrewrite_args.out_success) {
                rewrite_args = NULL;
            } else if (val) {
                rewrite_args->out_rtn = hrewrite_args.out_rtn;
                rewrite_args->out_success = true;
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
                rewrite_args->obj = rewrite_args->obj->getAttr(offsetof(BoxedClosure, parent));
            }
            return getattrInternal(closure->parent, attr, rewrite_args);
        }
        raiseExcHelper(NameError, "free variable '%s' referenced before assignment in enclosing scope", attr.c_str());
    }

    if (!cls_only) {
        // Don't need to pass icentry args, since we special-case __getattribtue__ and __getattr__ to use
        // invalidation rather than guards
        // TODO since you changed this to typeLookup you need to guard
        Box* getattribute = typeLookup(obj->cls, getattribute_str, NULL);
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
    RewriterVar* r_descr = NULL;
    if (rewrite_args) {
        RewriterVar* r_obj_cls = rewrite_args->obj->getAttr(BOX_CLS_OFFSET, Location::any());
        GetattrRewriteArgs grewrite_args(rewrite_args->rewriter, r_obj_cls, rewrite_args->destination);
        descr = typeLookup(obj->cls, attr, &grewrite_args);

        if (!grewrite_args.out_success) {
            rewrite_args = NULL;
        } else if (descr) {
            r_descr = grewrite_args.out_rtn;
        }
    } else {
        descr = typeLookup(obj->cls, attr, NULL);
    }

    // Check if it's a data descriptor
    Box* _get_ = NULL;
    RewriterVar* r_get = NULL;
    if (descr) {
        if (rewrite_args)
            r_descr->addAttrGuard(BOX_CLS_OFFSET, (uint64_t)descr->cls);

        // Special-case data descriptors (e.g., member descriptors)
        Box* res = dataDescriptorInstanceSpecialCases(rewrite_args, attr, obj, descr, r_descr, for_call, bind_obj_out,
                                                      r_bind_obj_out);
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
                RewriterVar* r_descr_cls = r_descr->getAttr(BOX_CLS_OFFSET, Location::any());
                GetattrRewriteArgs grewrite_args(rewrite_args->rewriter, r_descr_cls, Location::any());
                _get_ = typeLookup(descr->cls, get_str, &grewrite_args);
                if (!grewrite_args.out_success) {
                    rewrite_args = NULL;
                } else if (_get_) {
                    r_get = grewrite_args.out_rtn;
                }
            } else {
                _get_ = typeLookup(descr->cls, get_str, NULL);
            }

            // As an optimization, don't check for __set__ if we're in cls_only mode, since it won't matter.
            if (_get_ && !cls_only) {
                // Check if __set__ exists
                Box* _set_ = NULL;
                if (rewrite_args) {
                    RewriterVar* r_descr_cls = r_descr->getAttr(BOX_CLS_OFFSET, Location::any());
                    GetattrRewriteArgs grewrite_args(rewrite_args->rewriter, r_descr_cls, Location::any());
                    _set_ = typeLookup(descr->cls, set_str, &grewrite_args);
                    if (!grewrite_args.out_success) {
                        rewrite_args = NULL;
                    }
                } else {
                    _set_ = typeLookup(descr->cls, set_str, NULL);
                }

                // Call __get__(descr, obj, obj->cls)
                if (_set_) {
                    // Have to abort because we're about to call now, but there will be before more
                    // guards between this call and the next...
                    if (for_call) {
                        rewrite_args = NULL;
                        REWRITE_ABORTED("");
                    }

                    Box* res;
                    if (rewrite_args) {
                        CallRewriteArgs crewrite_args(rewrite_args->rewriter, r_get, rewrite_args->destination);
                        crewrite_args.arg1 = r_descr;
                        crewrite_args.arg2 = rewrite_args->obj;
                        crewrite_args.arg3 = rewrite_args->obj->getAttr(BOX_CLS_OFFSET, Location::any());
                        res = runtimeCallInternal(_get_, &crewrite_args, ArgPassSpec(3), descr, obj, obj->cls, NULL,
                                                  NULL);
                        if (!crewrite_args.out_success) {
                            rewrite_args = NULL;
                        } else {
                            rewrite_args->out_success = true;
                            rewrite_args->out_rtn = crewrite_args.out_rtn;
                        }
                    } else {
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
            RewriterVar* r_val = NULL;
            if (rewrite_args) {
                GetattrRewriteArgs hrewrite_args(rewrite_args->rewriter, rewrite_args->obj, rewrite_args->destination);
                val = obj->getattr(attr, &hrewrite_args);

                if (!hrewrite_args.out_success) {
                    rewrite_args = NULL;
                } else if (val) {
                    r_val = hrewrite_args.out_rtn;
                }
            } else {
                val = obj->getattr(attr, NULL);
            }

            if (val) {
                if (rewrite_args) {
                    rewrite_args->out_rtn = r_val;
                    rewrite_args->out_success = true;
                }
                return val;
            }
        } else {
            // More complicated when obj is a type
            // We have to look up the attr in the entire
            // class hierarchy, and we also have to check if it is a descriptor,
            // in addition to the data/nondata descriptor logic.
            // (in CPython, see type_getattro in typeobject.c)

            Box* val;
            RewriterVar* r_val = NULL;
            if (rewrite_args) {
                GetattrRewriteArgs grewrite_args(rewrite_args->rewriter, rewrite_args->obj, rewrite_args->destination);

                val = typeLookup(static_cast<BoxedClass*>(obj), attr, &grewrite_args);
                if (!grewrite_args.out_success) {
                    rewrite_args = NULL;
                } else if (val) {
                    r_val = grewrite_args.out_rtn;
                }
            } else {
                val = typeLookup(static_cast<BoxedClass*>(obj), attr, NULL);
            }

            if (val) {
                Box* res = descriptorClsSpecialCases(rewrite_args, static_cast<BoxedClass*>(obj), val, r_val, for_call,
                                                     bind_obj_out, r_bind_obj_out);
                if (res) {
                    return res;
                }

                // Lookup __get__
                RewriterVar* r_get = NULL;
                Box* local_get;
                if (rewrite_args) {
                    RewriterVar* r_val_cls = r_val->getAttr(BOX_CLS_OFFSET, Location::any());
                    GetattrRewriteArgs grewrite_args(rewrite_args->rewriter, r_val_cls, Location::any());
                    local_get = typeLookup(val->cls, get_str, &grewrite_args);
                    if (!grewrite_args.out_success) {
                        rewrite_args = NULL;
                    } else if (local_get) {
                        r_get = grewrite_args.out_rtn;
                    }
                } else {
                    local_get = typeLookup(val->cls, get_str, NULL);
                }

                // Call __get__(val, None, obj)
                if (local_get) {
                    Box* res;

                    if (for_call) {
                        rewrite_args = NULL;
                        REWRITE_ABORTED("");
                    }

                    if (rewrite_args) {
                        CallRewriteArgs crewrite_args(rewrite_args->rewriter, r_get, rewrite_args->destination);
                        crewrite_args.arg1 = r_val;
                        crewrite_args.arg2 = rewrite_args->rewriter->loadConst((intptr_t)None, Location::any());
                        crewrite_args.arg3 = rewrite_args->obj;
                        res = runtimeCallInternal(local_get, &crewrite_args, ArgPassSpec(3), val, None, obj, NULL,
                                                  NULL);
                        if (!crewrite_args.out_success) {
                            rewrite_args = NULL;
                        } else {
                            rewrite_args->out_success = true;
                            rewrite_args->out_rtn = crewrite_args.out_rtn;
                        }
                    } else {
                        res = runtimeCallInternal(local_get, NULL, ArgPassSpec(3), val, None, obj, NULL, NULL);
                    }
                    return res;
                }

                // If there was no local __get__, just return val
                if (rewrite_args) {
                    rewrite_args->out_rtn = r_val;
                    rewrite_args->out_success = true;
                }
                return val;
            }
        }
    }

    // If descr and __get__ exist, then call __get__
    if (descr) {
        // Special cases first
        Box* res = nondataDescriptorInstanceSpecialCases(rewrite_args, obj, descr, r_descr, for_call, bind_obj_out,
                                                         r_bind_obj_out);
        if (res) {
            return res;
        }

        // We looked up __get__ above. If we found it, call it and return
        // the result.
        if (_get_) {
            // this could happen for the callattr path...
            if (for_call) {
                rewrite_args = NULL;
                REWRITE_ABORTED("");
            }

            Box* res;
            if (rewrite_args) {
                CallRewriteArgs crewrite_args(rewrite_args->rewriter, r_get, rewrite_args->destination);
                crewrite_args.arg1 = r_descr;
                crewrite_args.arg2 = rewrite_args->obj;
                crewrite_args.arg3 = rewrite_args->obj->getAttr(BOX_CLS_OFFSET, Location::any());
                res = runtimeCallInternal(_get_, &crewrite_args, ArgPassSpec(3), descr, obj, obj->cls, NULL, NULL);
                if (!crewrite_args.out_success) {
                    rewrite_args = NULL;
                } else {
                    rewrite_args->out_success = true;
                    rewrite_args->out_rtn = crewrite_args.out_rtn;
                }
            } else {
                res = runtimeCallInternal(_get_, NULL, ArgPassSpec(3), descr, obj, obj->cls, NULL, NULL);
            }
            return res;
        }

        // Otherwise, just return descr.
        if (rewrite_args) {
            rewrite_args->out_rtn = r_descr;
            rewrite_args->out_success = true;
        }
        return descr;
    }

    // Finally, check __getattr__

    if (!cls_only) {
        // Don't need to pass icentry args, since we special-case __getattribute__ and __getattr__ to use
        // invalidation rather than guards
        rewrite_args = NULL;
        REWRITE_ABORTED("");

        if (obj->cls->tp_getattr) {
            Box* rtn = obj->cls->tp_getattr(obj, const_cast<char*>(attr.c_str()));
            if (rtn == NULL)
                throwCAPIException();
            return rtn;
        }
        Box* getattr = typeLookup(obj->cls, getattr_str, NULL);
        if (getattr) {
            Box* boxstr = boxString(attr);
            Box* rtn = runtimeCall2(getattr, ArgPassSpec(2), obj, boxstr);
            return rtn;
        }

        if (rewrite_args) {
            rewrite_args->rewriter->addDependenceOn(obj->cls->dependent_icgetattrs);
        }
    }

    if (rewrite_args) {
        rewrite_args->out_success = true;
    }
    return NULL;
}

Box* getattrInternal(Box* obj, const std::string& attr, GetattrRewriteArgs* rewrite_args) {
    return getattrInternalGeneral(obj, attr, rewrite_args,
                                  /* cls_only */ false,
                                  /* for_call */ false, NULL, NULL);
}

extern "C" Box* getattr(Box* obj, const char* attr) {
    static StatCounter slowpath_getattr("slowpath_getattr");
    slowpath_getattr.log();

    bool is_dunder = (attr[0] == '_' && attr[1] == '_');

    if (is_dunder) {
        if (strcmp(attr, "__dict__") == 0) {
            // TODO this is wrong, should be added at the class level as a getset
            if (obj->cls->instancesHaveHCAttrs())
                return makeAttrWrapper(obj);
        }
    }

    if (VERBOSITY() >= 2) {
#if !DISABLE_STATS
        std::string per_name_stat_name = "getattr__" + std::string(attr);
        int id = Stats::getStatId(per_name_stat_name);
        Stats::log(id);
#endif
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
        GetattrRewriteArgs rewrite_args(rewriter.get(), rewriter->getArg(0), dest);
        val = getattrInternal(obj, attr, &rewrite_args);

        // should make sure getattrInternal calls finishes using obj itself
        // if it is successful

        if (rewrite_args.out_success && val) {
            if (recorder) {
                RewriterVar* record_rtn = rewriter->call(false, (void*)recordType,
                                                         rewriter->loadConst((intptr_t)recorder, Location::forArg(0)),
                                                         rewrite_args.out_rtn);
                rewriter->commitReturning(record_rtn);

                recordType(recorder, val);
            } else {
                rewriter->commitReturning(rewrite_args.out_rtn);
            }
        }
    } else {
        val = getattrInternal(obj, attr, NULL);
    }

    if (val) {
        return val;
    }

    if (is_dunder) {
        // There's more to it than this:
        if (strcmp(attr, "__class__") == 0) {
            assert(obj->cls != instance_cls); // I think in this case __class__ is supposed to be the classobj?
            return obj->cls;
        }

        // This doesn't belong here either:
        if (strcmp(attr, "__bases__") == 0 && isSubclass(obj->cls, type_cls)) {
            BoxedClass* cls = static_cast<BoxedClass*>(obj);
            if (cls->tp_base)
                return new BoxedTuple({ static_cast<BoxedClass*>(obj)->tp_base });
            return EmptyTuple;
        }
    }

    raiseAttributeError(obj, attr);
}

bool dataDescriptorSetSpecialCases(Box* obj, Box* val, Box* descr, SetattrRewriteArgs* rewrite_args,
                                   RewriterVar* r_descr, const std::string& attr_name) {

    // Special case: getset descriptor
    if (descr->cls == getset_cls) {
        BoxedGetsetDescriptor* getset_descr = static_cast<BoxedGetsetDescriptor*>(descr);

        // TODO type checking goes here
        if (getset_descr->set == NULL) {
            raiseExcHelper(AttributeError, "attribute '%s' of '%s' object is not writable", attr_name.c_str(),
                           getTypeName(getset_descr));
        }

        if (rewrite_args) {
            RewriterVar* r_obj = rewrite_args->obj;
            RewriterVar* r_val = rewrite_args->attrval;

            r_descr->addAttrGuard(offsetof(BoxedGetsetDescriptor, set), (intptr_t)getset_descr->set);
            RewriterVar* r_closure = r_descr->getAttr(offsetof(BoxedGetsetDescriptor, closure));
            rewrite_args->rewriter->call(
                /* can_call_into_python */ true, (void*)getset_descr->set, { r_obj, r_val, r_closure });
            rewrite_args->out_success = true;
        }

        getset_descr->set(obj, val, getset_descr->closure);

        return true;
    }

    return false;
}

void setattrInternal(Box* obj, const std::string& attr, Box* val, SetattrRewriteArgs* rewrite_args) {
    assert(gc::isValidGCObject(val));

    // Lookup a descriptor
    Box* descr = NULL;
    RewriterVar* r_descr = NULL;
    // TODO probably check that the cls is user-defined or something like that
    // (figure out exactly what)
    // (otherwise no need to check descriptor logic)
    if (rewrite_args) {
        RewriterVar* r_cls = rewrite_args->obj->getAttr(BOX_CLS_OFFSET, Location::any());
        GetattrRewriteArgs crewrite_args(rewrite_args->rewriter, r_cls, rewrite_args->rewriter->getReturnDestination());
        descr = typeLookup(obj->cls, attr, &crewrite_args);

        if (!crewrite_args.out_success) {
            rewrite_args = NULL;
        } else if (descr) {
            r_descr = crewrite_args.out_rtn;
        }
    } else {
        descr = typeLookup(obj->cls, attr, NULL);
    }

    Box* _set_ = NULL;
    RewriterVar* r_set = NULL;
    if (descr) {
        bool special_case_worked = dataDescriptorSetSpecialCases(obj, val, descr, rewrite_args, r_descr, attr);
        if (special_case_worked) {
            // We don't need to to the invalidation stuff in this case.
            return;
        }

        if (rewrite_args) {
            RewriterVar* r_cls = r_descr->getAttr(BOX_CLS_OFFSET, Location::any());
            GetattrRewriteArgs trewrite_args(rewrite_args->rewriter, r_cls, Location::any());
            _set_ = typeLookup(descr->cls, set_str, &trewrite_args);
            if (!trewrite_args.out_success) {
                rewrite_args = NULL;
            } else if (_set_) {
                r_set = trewrite_args.out_rtn;
            }
        } else {
            _set_ = typeLookup(descr->cls, set_str, NULL);
        }
    }

    // If `descr` has __set__ (thus making it a descriptor) we should call
    // __set__ with `val` rather than directly calling setattr
    if (descr && _set_) {
        if (rewrite_args) {
            CallRewriteArgs crewrite_args(rewrite_args->rewriter, r_set, Location::any());
            crewrite_args.arg1 = r_descr;
            crewrite_args.arg2 = rewrite_args->obj;
            crewrite_args.arg3 = rewrite_args->attrval;
            runtimeCallInternal(_set_, &crewrite_args, ArgPassSpec(3), descr, obj, val, NULL, NULL);
            if (crewrite_args.out_success) {
                rewrite_args->out_success = true;
            }
        } else {
            runtimeCallInternal(_set_, NULL, ArgPassSpec(3), descr, obj, val, NULL, NULL);
        }

        // We don't need to to the invalidation stuff in this case.
        return;
    } else {
        obj->setattr(attr, val, rewrite_args);
    }

    if (isSubclass(obj->cls, type_cls)) {
        BoxedClass* self = static_cast<BoxedClass*>(obj);

        if (attr == getattr_str || attr == getattribute_str) {
            if (rewrite_args)
                REWRITE_ABORTED("");
            // Will have to embed the clear in the IC, so just disable the patching for now:
            rewrite_args = NULL;

            // TODO should put this clearing behavior somewhere else, since there are probably more
            // cases in which we want to do it.
            self->dependent_icgetattrs.invalidateAll();
        }

        if (attr == "__base__" && self->getattr("__base__"))
            raiseExcHelper(TypeError, "readonly attribute");

        bool touched_slot = update_slot(self, attr);
        if (touched_slot) {
            rewrite_args = NULL;
            REWRITE_ABORTED("");
        }
    }
}

extern "C" void setattr(Box* obj, const char* attr, Box* attr_val) {
    assert(strcmp(attr, "__class__") != 0);

    static StatCounter slowpath_setattr("slowpath_setattr");
    slowpath_setattr.log();

    if (!obj->cls->instancesHaveHCAttrs() && !obj->cls->instancesHaveDictAttrs()) {
        raiseAttributeError(obj, attr);
    }

    if (obj->cls == type_cls) {
        BoxedClass* cobj = static_cast<BoxedClass*>(obj);
        if (!isUserDefined(cobj)) {
            raiseExcHelper(TypeError, "can't set attributes of built-in/extension type '%s'", getNameOfClass(cobj));
        }
    }

    std::unique_ptr<Rewriter> rewriter(
        Rewriter::createRewriter(__builtin_extract_return_addr(__builtin_return_address(0)), 3, "setattr"));

    if (rewriter.get()) {
        // rewriter->trap();
        SetattrRewriteArgs rewrite_args(rewriter.get(), rewriter->getArg(0), rewriter->getArg(2));
        setattrInternal(obj, attr, attr_val, &rewrite_args);
        if (rewrite_args.out_success) {
            rewriter->commit();
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
    assert(gc::isValidGCObject(obj));

    static StatCounter slowpath_nonzero("slowpath_nonzero");

    std::unique_ptr<Rewriter> rewriter(
        Rewriter::createRewriter(__builtin_extract_return_addr(__builtin_return_address(0)), 1, "nonzero"));

    RewriterVar* r_obj = NULL;
    if (rewriter.get()) {
        r_obj = rewriter->getArg(0);
        r_obj->addAttrGuard(BOX_CLS_OFFSET, (intptr_t)obj->cls);
    }

    if (obj->cls == bool_cls) {
        // TODO: is it faster to compare to True? (especially since it will be a constant we can embed in the rewrite)
        if (rewriter.get()) {
            RewriterVar* b = r_obj->getAttr(BOOL_B_OFFSET, rewriter->getReturnDestination());
            rewriter->commitReturning(b);
        }

        BoxedBool* bool_obj = static_cast<BoxedBool*>(obj);
        return bool_obj->n;
    } else if (obj->cls == int_cls) {
        if (rewriter.get()) {
            // TODO should do:
            // test 	%rsi, %rsi
            // setne	%al
            RewriterVar* n = r_obj->getAttr(INT_N_OFFSET, rewriter->getReturnDestination());
            RewriterVar* b = n->toBool(rewriter->getReturnDestination());
            rewriter->commitReturning(b);
        }

        BoxedInt* int_obj = static_cast<BoxedInt*>(obj);
        return int_obj->n != 0;
    } else if (obj->cls == float_cls) {
        if (rewriter.get()) {
            RewriterVar* b = rewriter->call(false, (void*)floatNonzeroUnboxed, r_obj);
            rewriter->commitReturning(b);
        }
        return static_cast<BoxedFloat*>(obj)->d != 0;
    } else if (obj->cls == none_cls) {
        if (rewriter.get()) {
            RewriterVar* b = rewriter->loadConst(0, rewriter->getReturnDestination());
            rewriter->commitReturning(b);
        }
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

    // go through descriptor logic
    Box* func = getclsattr_internal(obj, "__nonzero__", NULL);
    if (!func)
        func = getclsattr_internal(obj, "__len__", NULL);

    if (func == NULL) {
        ASSERT(isUserDefined(obj->cls) || obj->cls == classobj_cls || obj->cls == type_cls
                   || isSubclass(obj->cls, Exception) || obj->cls == file_cls || obj->cls == traceback_cls
                   || obj->cls == instancemethod_cls || obj->cls == classmethod_cls || obj->cls == module_cls,
               "%s.__nonzero__", getTypeName(obj)); // TODO

        // TODO should rewrite these?
        return true;
    }

    Box* r = runtimeCall0(func, ArgPassSpec(0));
    // I believe this behavior is handled by the slot wrappers in CPython:
    if (r->cls == bool_cls) {
        BoxedBool* b = static_cast<BoxedBool*>(r);
        bool rtn = b->n;
        return rtn;
    } else if (r->cls == int_cls) {
        BoxedInt* b = static_cast<BoxedInt*>(r);
        bool rtn = b->n != 0;
        return rtn;
    } else {
        raiseExcHelper(TypeError, "__nonzero__ should return bool or int, returned %s", getTypeName(r));
    }
}

extern "C" BoxedString* str(Box* obj) {
    static StatCounter slowpath_str("slowpath_str");
    slowpath_str.log();

    if (obj->cls != str_cls) {
        // TODO could do an IC optimization here (once we do rewrites here at all):
        // if __str__ is objectStr, just guard on that and call repr directly.
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
    } catch (ExcInfo e) {
        return nullptr;
    }
}

extern "C" BoxedString* strOrNull(Box* obj) {
    try {
        BoxedString* r = str(obj);
        return static_cast<BoxedString*>(r);
    } catch (ExcInfo e) {
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
        ASSERT(isUserDefined(obj->cls), "%s.__hash__", getTypeName(obj));
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
    if (rewrite_args) {
        CallRewriteArgs crewrite_args(rewrite_args->rewriter, rewrite_args->obj, rewrite_args->destination);
        rtn = callattrInternal0(obj, &attr_str, CLASS_ONLY, &crewrite_args, ArgPassSpec(0));
        if (!crewrite_args.out_success)
            rewrite_args = NULL;
        else if (rtn)
            rewrite_args->out_rtn = crewrite_args.out_rtn;
    } else {
        rtn = callattrInternal0(obj, &attr_str, CLASS_ONLY, NULL, ArgPassSpec(0));
    }

    if (rtn == NULL) {
        raiseExcHelper(TypeError, "object of type '%s' has no len()", getTypeName(obj));
    }

    if (rtn->cls != int_cls) {
        raiseExcHelper(TypeError, "an integer is required");
    }

    if (rewrite_args)
        rewrite_args->out_success = true;
    return static_cast<BoxedInt*>(rtn);
}

Box* lenCallInternal(BoxedFunctionBase* func, CallRewriteArgs* rewrite_args, ArgPassSpec argspec, Box* arg1, Box* arg2,
                     Box* arg3, Box** args, const std::vector<const std::string*>* keyword_names) {
    if (argspec != ArgPassSpec(1))
        return callFunc(func, rewrite_args, argspec, arg1, arg2, arg3, args, keyword_names);

    if (rewrite_args) {
        LenRewriteArgs lrewrite_args(rewrite_args->rewriter, rewrite_args->arg1, rewrite_args->destination);
        Box* rtn = lenInternal(arg1, &lrewrite_args);
        if (!lrewrite_args.out_success) {
            rewrite_args = 0;
        } else {
            rewrite_args->out_rtn = lrewrite_args.out_rtn;
            rewrite_args->out_success = true;
        }
        return rtn;
    }
    return lenInternal(arg1, NULL);
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
    RewriterVar* r_boxed = NULL;
    if (rewriter.get()) {
        // rewriter->trap();
        LenRewriteArgs rewrite_args(rewriter.get(), rewriter->getArg(0), rewriter->getReturnDestination());
        lobj = lenInternal(obj, &rewrite_args);

        if (!rewrite_args.out_success) {
            rewriter.reset(NULL);
        } else
            r_boxed = rewrite_args.out_rtn;
    } else {
        lobj = lenInternal(obj, NULL);
    }

    assert(lobj->cls == int_cls);
    i64 rtn = lobj->n;

    if (rewriter.get()) {
        RewriterVar* rtn = r_boxed->getAttr(INT_N_OFFSET, Location(assembler::RAX));
        rewriter->commitReturning(rtn);
    }
    return rtn;
}

extern "C" void dump(void* p) {
    printf("\n");
    printf("Raw address: %p\n", p);

    bool is_gc = gc::isValidGCObject(p);
    if (!is_gc) {
        printf("non-gc memory\n");
        return;
    }

    if (gc::isNonheapRoot(p)) {
        printf("Non-heap GC object\n");

        printf("Assuming it's a class object...\n");
        PyTypeObject* type = (PyTypeObject*)(p);
        printf("tp_name: %s\n", type->tp_name);
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

        if (b->cls == bool_cls) {
            printf("The %s object\n", b == True ? "True" : "False");
        }

        if (isSubclass(b->cls, type_cls)) {
            printf("Type name: %s\n", getFullNameOfClass(static_cast<BoxedClass*>(b)).c_str());
        }

        if (isSubclass(b->cls, str_cls)) {
            printf("String value: %s\n", static_cast<BoxedString*>(b)->s.c_str());
        }

        if (isSubclass(b->cls, tuple_cls)) {
            printf("%ld elements\n", static_cast<BoxedTuple*>(b)->elts.size());
        }

        if (isSubclass(b->cls, int_cls)) {
            printf("Int value: %ld\n", static_cast<BoxedInt*>(b)->n);
        }

        if (isSubclass(b->cls, list_cls)) {
            printf("%ld elements\n", static_cast<BoxedList*>(b)->size);
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
            rewrite_args->arg1->addAttrGuard(BOX_CLS_OFFSET, (intptr_t)arg1->cls);
        if (npassed_args >= 2)
            rewrite_args->arg2->addAttrGuard(BOX_CLS_OFFSET, (intptr_t)arg2->cls);
        if (npassed_args >= 3)
            rewrite_args->arg3->addAttrGuard(BOX_CLS_OFFSET, (intptr_t)arg3->cls);

        if (npassed_args > 3) {
            for (int i = 3; i < npassed_args; i++) {
                // TODO if there are a lot of args (>16), might be better to increment a pointer
                // rather index them directly?
                RewriterVar* v = rewrite_args->args->getAttr((i - 3) * sizeof(Box*), Location::any());
                v->addAttrGuard(BOX_CLS_OFFSET, (intptr_t)args[i - 3]->cls);
            }
        }
    }

    // right now I don't think this is ever called with INST_ONLY?
    assert(scope != INST_ONLY);

    // Look up the argument. Pass in the arguments to getattrInternalGeneral or getclsattr_general
    // that will shortcut functions by not putting them into instancemethods
    Box* bind_obj;
    RewriterVar* r_bind_obj;
    Box* val;
    RewriterVar* r_val = NULL;
    if (rewrite_args) {
        GetattrRewriteArgs grewrite_args(rewrite_args->rewriter, rewrite_args->obj, Location::any());
        val = getattrInternalGeneral(obj, *attr, &grewrite_args, scope == CLASS_ONLY, true, &bind_obj, &r_bind_obj);
        if (!grewrite_args.out_success) {
            rewrite_args = NULL;
        } else if (val) {
            r_val = grewrite_args.out_rtn;
        }
    } else {
        val = getattrInternalGeneral(obj, *attr, NULL, scope == CLASS_ONLY, true, &bind_obj, &r_bind_obj);
    }

    if (val == NULL) {
        if (rewrite_args)
            rewrite_args->out_success = true;
        return val;
    }

    if (bind_obj != NULL) {
        if (rewrite_args) {
            r_val->addGuard((int64_t)val);
        }

        // TODO copy from runtimeCall
        // TODO these two branches could probably be folded together (the first one is becoming
        // a subset of the second)
        if (npassed_args <= 2) {
            Box* rtn;
            if (rewrite_args) {
                CallRewriteArgs srewrite_args(rewrite_args->rewriter, r_val, rewrite_args->destination);
                srewrite_args.arg1 = r_bind_obj;

                // should be no-ops:
                if (npassed_args >= 1)
                    srewrite_args.arg2 = rewrite_args->arg1;
                if (npassed_args >= 2)
                    srewrite_args.arg3 = rewrite_args->arg2;

                srewrite_args.func_guarded = true;
                srewrite_args.args_guarded = true;

                rtn = runtimeCallInternal(val, &srewrite_args, ArgPassSpec(argspec.num_args + 1, argspec.num_keywords,
                                                                           argspec.has_starargs, argspec.has_kwargs),
                                          bind_obj, arg1, arg2, NULL, keyword_names);

                if (!srewrite_args.out_success) {
                    rewrite_args = NULL;
                } else {
                    rewrite_args->out_rtn = srewrite_args.out_rtn;
                }
            } else {
                rtn = runtimeCallInternal(val, NULL, ArgPassSpec(argspec.num_args + 1, argspec.num_keywords,
                                                                 argspec.has_starargs, argspec.has_kwargs),
                                          bind_obj, arg1, arg2, NULL, keyword_names);
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
                CallRewriteArgs srewrite_args(rewrite_args->rewriter, r_val, rewrite_args->destination);
                srewrite_args.arg1 = r_bind_obj;
                srewrite_args.arg2 = rewrite_args->arg1;
                srewrite_args.arg3 = rewrite_args->arg2;
                srewrite_args.args = rewrite_args->rewriter->allocateAndCopyPlus1(
                    rewrite_args->arg3, npassed_args == 3 ? NULL : rewrite_args->args, npassed_args - 3);

                srewrite_args.args_guarded = true;
                srewrite_args.func_guarded = true;

                rtn = runtimeCallInternal(val, &srewrite_args, ArgPassSpec(argspec.num_args + 1, argspec.num_keywords,
                                                                           argspec.has_starargs, argspec.has_kwargs),
                                          bind_obj, arg1, arg2, new_args, keyword_names);

                if (!srewrite_args.out_success) {
                    rewrite_args = NULL;
                } else {
                    rewrite_args->out_rtn = srewrite_args.out_rtn;

                    rewrite_args->out_success = true;
                }
            } else {
                rtn = runtimeCallInternal(val, NULL, ArgPassSpec(argspec.num_args + 1, argspec.num_keywords,
                                                                 argspec.has_starargs, argspec.has_kwargs),
                                          bind_obj, arg1, arg2, new_args, keyword_names);
            }
            return rtn;
        }
    } else {
        Box* rtn;
        // I *think* this check is here to limit the recursion nesting for rewriting, and originates
        // from a time when we didn't have silent-abort-when-patchpoint-full.
        if (val->cls != function_cls && val->cls != builtin_function_or_method_cls && val->cls != instancemethod_cls
            && val->cls != classmethod_cls && val->cls != capifunc_cls) {
            rewrite_args = NULL;
            REWRITE_ABORTED("");
        }

        if (rewrite_args) {
            CallRewriteArgs srewrite_args(rewrite_args->rewriter, r_val, rewrite_args->destination);
            if (npassed_args >= 1)
                srewrite_args.arg1 = rewrite_args->arg1;
            if (npassed_args >= 2)
                srewrite_args.arg2 = rewrite_args->arg2;
            if (npassed_args >= 3)
                srewrite_args.arg3 = rewrite_args->arg3;
            if (npassed_args >= 4)
                srewrite_args.args = rewrite_args->args;
            srewrite_args.args_guarded = true;

            rtn = runtimeCallInternal(val, &srewrite_args, argspec, arg1, arg2, arg3, args, keyword_names);

            if (!srewrite_args.out_success) {
                rewrite_args = NULL;
            } else {
                rewrite_args->out_rtn = srewrite_args.out_rtn;
            }
        } else {
            rtn = runtimeCallInternal(val, NULL, argspec, arg1, arg2, arg3, args, keyword_names);
        }

        if (!rtn) {
            raiseExcHelper(TypeError, "'%s' object is not callable", getTypeName(val));
        }

        if (rewrite_args)
            rewrite_args->out_success = true;
        return rtn;
    }
}

extern "C" Box* callattr(Box* obj, const std::string* attr, CallattrFlags flags, ArgPassSpec argspec, Box* arg1,
                         Box* arg2, Box* arg3, Box** args, const std::vector<const std::string*>* keyword_names) {
    assert(gc::isValidGCObject(obj));

    int npassed_args = argspec.totalPassed();

    static StatCounter slowpath_callattr("slowpath_callattr");
    slowpath_callattr.log();

    assert(attr);

    int num_orig_args = 4 + std::min(4, npassed_args);
    if (argspec.num_keywords)
        num_orig_args++;

    // Uncomment this to help debug if callsites aren't getting rewritten:
    // printf("Slowpath call: %p (%s.%s)\n", __builtin_return_address(0), obj->cls->tp_name, attr->c_str());

    std::unique_ptr<Rewriter> rewriter(Rewriter::createRewriter(
        __builtin_extract_return_addr(__builtin_return_address(0)), num_orig_args, "callattr"));
    Box* rtn;

    LookupScope scope = flags.cls_only ? CLASS_ONLY : CLASS_OR_INST;

    if (rewriter.get()) {
        // TODO feel weird about doing this; it either isn't necessary
        // or this kind of thing is necessary in a lot more places
        // rewriter->getArg(3).addGuard(npassed_args);

        CallRewriteArgs rewrite_args(rewriter.get(), rewriter->getArg(0), rewriter->getReturnDestination());
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
            rewriter->commitReturning(rewrite_args.out_rtn);
        }
    } else {
        rtn = callattrInternal(obj, attr, scope, NULL, argspec, arg1, arg2, arg3, args, keyword_names);
    }

    if (rtn == NULL && !flags.null_on_nonexistent) {
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

        EffortLevel new_effort = initialEffort();

        // this also pushes the new CompiledVersion to the back of the version list:
        chosen_cf = compileFunction(f, spec, new_effort, NULL);
    }

    return chosen_cf;
}

static std::string getFunctionName(CLFunction* f) {
    if (f->source)
        return f->source->getName();
    else if (f->versions.size()) {
        std::ostringstream oss;
        oss << "<function at " << f->versions[0]->code << ">";
        return oss.str();
    }
    return "<unknown function>";
}

enum class KeywordDest {
    POSITIONAL,
    KWARGS,
};
static KeywordDest placeKeyword(const ParamNames& param_names, std::vector<bool>& params_filled,
                                const std::string& kw_name, Box* kw_val, Box*& oarg1, Box*& oarg2, Box*& oarg3,
                                Box** oargs, BoxedDict* okwargs, CLFunction* cl) {
    assert(kw_val);

    for (int j = 0; j < param_names.args.size(); j++) {
        if (param_names.args[j].str() == kw_name && kw_name.size() > 0) {
            if (params_filled[j]) {
                raiseExcHelper(TypeError, "%.200s() got multiple values for keyword argument '%s'",
                               getFunctionName(cl).c_str(), kw_name.c_str());
            }

            getArg(j, oarg1, oarg2, oarg3, oargs) = kw_val;
            params_filled[j] = true;

            return KeywordDest::POSITIONAL;
        }
    }

    if (okwargs) {
        Box*& v = okwargs->d[boxString(kw_name)];
        if (v) {
            raiseExcHelper(TypeError, "%.200s() got multiple values for keyword argument '%s'",
                           getFunctionName(cl).c_str(), kw_name.c_str());
        }
        v = kw_val;
        return KeywordDest::KWARGS;
    } else {
        raiseExcHelper(TypeError, "%.200s() got an unexpected keyword argument '%s'", getFunctionName(cl).c_str(),
                       kw_name.c_str());
    }
}

Box* callFunc(BoxedFunctionBase* func, CallRewriteArgs* rewrite_args, ArgPassSpec argspec, Box* arg1, Box* arg2,
              Box* arg3, Box** args, const std::vector<const std::string*>* keyword_names) {

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

    if (argspec.has_starargs || argspec.has_kwargs || func->isGenerator) {
        rewrite_args = NULL;
        REWRITE_ABORTED("");
    }

    // These could be handled:
    if (argspec.num_keywords) {
        rewrite_args = NULL;
        REWRITE_ABORTED("");
    }

    // TODO Should we guard on the CLFunction or the BoxedFunctionBase?
    // A single CLFunction could end up forming multiple BoxedFunctionBases, and we
    // could emit assembly that handles any of them.  But doing this involves some
    // extra indirection, and it's not clear if that's worth it, since it seems like
    // the common case will be functions only ever getting a single set of default arguments.
    bool guard_clfunc = false;
    assert(!guard_clfunc && "I think there are users that expect the boxedfunction to be guarded");

    if (rewrite_args) {
        assert(rewrite_args->args_guarded && "need to guard args here");

        if (!rewrite_args->func_guarded) {
            if (guard_clfunc) {
                rewrite_args->obj->addAttrGuard(offsetof(BoxedFunctionBase, f), (intptr_t)f);
            } else {
                rewrite_args->obj->addGuard((intptr_t)func);
            }
            rewrite_args->rewriter->addDependenceOn(func->dependent_ics);
        }
    }

    if (rewrite_args) {
        // We might have trouble if we have more output args than input args,
        // such as if we need more space to pass defaults.
        if (num_output_args > 3 && num_output_args > argspec.totalPassed()) {
            int arg_bytes_required = (num_output_args - 3) * sizeof(Box*);
            RewriterVar* new_args = NULL;
            if (rewrite_args->args == NULL) {
                // rewrite_args->args could be empty if there are not more than
                // 3 input args.
                new_args = rewrite_args->rewriter->allocate(num_output_args - 3);
            } else {
                new_args = rewrite_args->rewriter->allocateAndCopy(rewrite_args->args, num_output_args - 3);
            }

            rewrite_args->args = new_args;
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
    Box* oarg1 = NULL, * oarg2 = NULL, * oarg3 = NULL;
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

    std::vector<Box*, StlCompatAllocator<Box*>> unused_positional;
    for (int i = positional_to_positional; i < argspec.num_args; i++) {
        rewrite_args = NULL;
        REWRITE_ABORTED("");
        unused_positional.push_back(getArg(i, arg1, arg2, arg3, args));
    }
    for (int i = varargs_to_positional; i < varargs.size(); i++) {
        rewrite_args = NULL;
        REWRITE_ABORTED("");
        unused_positional.push_back(varargs[i]);
    }

    if (f->takes_varargs) {
        int varargs_idx = f->num_args;
        if (rewrite_args) {
            assert(!unused_positional.size());
            // rewrite_args->rewriter->loadConst((intptr_t)EmptyTuple, Location::forArg(varargs_idx));
            RewriterVar* emptyTupleConst = rewrite_args->rewriter->loadConst(
                (intptr_t)EmptyTuple, varargs_idx < 3 ? Location::forArg(varargs_idx) : Location::any());
            if (varargs_idx == 0)
                rewrite_args->arg1 = emptyTupleConst;
            if (varargs_idx == 1)
                rewrite_args->arg2 = emptyTupleConst;
            if (varargs_idx == 2)
                rewrite_args->arg3 = emptyTupleConst;
            if (varargs_idx >= 3)
                rewrite_args->args->setAttr((varargs_idx - 3) * sizeof(Box*), emptyTupleConst);
        }

        Box* ovarargs = new BoxedTuple(unused_positional);
        getArg(varargs_idx, oarg1, oarg2, oarg3, oargs) = ovarargs;
    } else if (unused_positional.size()) {
        raiseExcHelper(TypeError, "%s() takes at most %d argument%s (%d given)", getFunctionName(f).c_str(),
                       f->num_args, (f->num_args == 1 ? "" : "s"),
                       argspec.num_args + argspec.num_keywords + varargs.size());
    }

    ////
    // Second, apply any keywords:

    BoxedDict* okwargs = NULL;
    if (f->takes_kwargs) {
        int kwargs_idx = f->num_args + (f->takes_varargs ? 1 : 0);
        if (rewrite_args) {
            assert(!unused_positional.size());
            RewriterVar* r_kwargs = rewrite_args->rewriter->call(false, (void*)createDict);

            if (kwargs_idx == 0)
                rewrite_args->arg1 = r_kwargs;
            if (kwargs_idx == 1)
                rewrite_args->arg2 = r_kwargs;
            if (kwargs_idx == 2)
                rewrite_args->arg3 = r_kwargs;
            if (kwargs_idx >= 3)
                rewrite_args->args->setAttr((kwargs_idx - 3) * sizeof(Box*), r_kwargs);
        }

        okwargs = new BoxedDict();
        getArg(kwargs_idx, oarg1, oarg2, oarg3, oargs) = okwargs;
    }

    const ParamNames& param_names = f->param_names;
    if (!param_names.takes_param_names && argspec.num_keywords && !f->takes_kwargs) {
        raiseExcHelper(TypeError, "%s() doesn't take keyword arguments", getFunctionName(f).c_str());
    }

    if (argspec.num_keywords)
        assert(argspec.num_keywords == keyword_names->size());

    for (int i = 0; i < argspec.num_keywords; i++) {
        assert(!rewrite_args && "would need to be handled here");

        int arg_idx = i + argspec.num_args;
        Box* kw_val = getArg(arg_idx, arg1, arg2, arg3, args);

        if (!param_names.takes_param_names) {
            assert(okwargs);
            rewrite_args = NULL; // would need to add it to r_kwargs
            okwargs->d[boxStringPtr((*keyword_names)[i])] = kw_val;
            continue;
        }

        auto dest = placeKeyword(param_names, params_filled, *(*keyword_names)[i], kw_val, oarg1, oarg2, oarg3, oargs,
                                 okwargs, f);
        if (dest == KeywordDest::KWARGS)
            rewrite_args = NULL;
    }

    if (argspec.has_kwargs) {
        assert(!rewrite_args && "would need to be handled here");

        Box* kwargs
            = getArg(argspec.num_args + argspec.num_keywords + (argspec.has_starargs ? 1 : 0), arg1, arg2, arg3, args);
        RELEASE_ASSERT(kwargs->cls == dict_cls, "haven't implemented this for non-dicts");
        BoxedDict* d_kwargs = static_cast<BoxedDict*>(kwargs);

        for (auto& p : d_kwargs->d) {
            if (p.first->cls != str_cls)
                raiseExcHelper(TypeError, "%s() keywords must be strings", getFunctionName(f).c_str());

            BoxedString* s = static_cast<BoxedString*>(p.first);

            if (param_names.takes_param_names) {
                assert(!rewrite_args && "would need to make sure that this didn't need to go into r_kwargs");
                placeKeyword(param_names, params_filled, s->s, p.second, oarg1, oarg2, oarg3, oargs, okwargs, f);
            } else {
                assert(!rewrite_args && "would need to make sure that this didn't need to go into r_kwargs");
                assert(okwargs);

                Box*& v = okwargs->d[p.first];
                if (v) {
                    raiseExcHelper(TypeError, "%s() got multiple values for keyword argument '%s'",
                                   getFunctionName(f).c_str(), s->s.c_str());
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
        raiseExcHelper(TypeError, "%s() did not get a value for positional argument %d", getFunctionName(f).c_str(), i);
    }

    RewriterVar* r_defaults_array = NULL;
    if (guard_clfunc) {
        r_defaults_array = rewrite_args->obj->getAttr(offsetof(BoxedFunctionBase, defaults), Location::any());
    }

    for (int i = f->num_args - f->num_defaults; i < f->num_args; i++) {
        if (params_filled[i])
            continue;

        int default_idx = i + f->num_defaults - f->num_args;
        Box* default_obj = func->defaults->elts[default_idx];

        if (rewrite_args) {
            int offset = offsetof(std::remove_pointer<decltype(BoxedFunctionBase::defaults)>::type, elts)
                         + sizeof(Box*) * default_idx;
            if (guard_clfunc) {
                // If we just guarded on the CLFunction, then we have to emit assembly
                // to fetch the values from the defaults array:
                if (i < 3) {
                    RewriterVar* r_default = r_defaults_array->getAttr(offset, Location::forArg(i));
                    if (i == 0)
                        rewrite_args->arg1 = r_default;
                    if (i == 1)
                        rewrite_args->arg2 = r_default;
                    if (i == 2)
                        rewrite_args->arg3 = r_default;
                } else {
                    RewriterVar* r_default = r_defaults_array->getAttr(offset, Location::any());
                    rewrite_args->args->setAttr((i - 3) * sizeof(Box*), r_default);
                }
            } else {
                // If we guarded on the BoxedFunctionBase, which has a constant set of defaults,
                // we can embed the default arguments directly into the instructions.
                if (i < 3) {
                    RewriterVar* r_default = rewrite_args->rewriter->loadConst((intptr_t)default_obj, Location::any());
                    if (i == 0)
                        rewrite_args->arg1 = r_default;
                    if (i == 1)
                        rewrite_args->arg2 = r_default;
                    if (i == 2)
                        rewrite_args->arg3 = r_default;
                } else {
                    RewriterVar* r_default = rewrite_args->rewriter->loadConst((intptr_t)default_obj, Location::any());
                    rewrite_args->args->setAttr((i - 3) * sizeof(Box*), r_default);
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
        return astInterpretFunction(chosen_cf, num_output_args, closure, generator, oarg1, oarg2, oarg3, oargs);
    }

    if (rewrite_args) {
        rewrite_args->rewriter->addDependenceOn(chosen_cf->dependent_callsites);

        std::vector<RewriterVar*> arg_vec;
        // TODO this kind of embedded reference needs to be tracked by the GC somehow?
        // Or maybe it's ok, since we've guarded on the function object?
        if (closure)
            arg_vec.push_back(rewrite_args->rewriter->loadConst((intptr_t)closure, Location::forArg(0)));
        if (num_output_args >= 1)
            arg_vec.push_back(rewrite_args->arg1);
        if (num_output_args >= 2)
            arg_vec.push_back(rewrite_args->arg2);
        if (num_output_args >= 3)
            arg_vec.push_back(rewrite_args->arg3);
        if (num_output_args >= 4)
            arg_vec.push_back(rewrite_args->args);

        rewrite_args->out_rtn = rewrite_args->rewriter->call(true, (void*)chosen_cf->call, arg_vec);
        rewrite_args->out_success = true;
    }

    Box* r;
    if (closure && generator)
        r = chosen_cf->closure_generator_call(closure, generator, oarg1, oarg2, oarg3, oargs);
    else if (closure)
        r = chosen_cf->closure_call(closure, oarg1, oarg2, oarg3, oargs);
    else if (generator)
        r = chosen_cf->generator_call(generator, oarg1, oarg2, oarg3, oargs);
    else
        r = chosen_cf->call(oarg1, oarg2, oarg3, oargs);

    ASSERT(chosen_cf->spec->rtn_type->isFitBy(r->cls), "%s (%p) %s %s",
           g.func_addr_registry.getFuncNameAtAddress(chosen_cf->code, true, NULL).c_str(), chosen_cf->code,
           chosen_cf->spec->rtn_type->debugName().c_str(), r->cls->tp_name);
    return r;
}


Box* runtimeCallInternal(Box* obj, CallRewriteArgs* rewrite_args, ArgPassSpec argspec, Box* arg1, Box* arg2, Box* arg3,
                         Box** args, const std::vector<const std::string*>* keyword_names) {
    int npassed_args = argspec.totalPassed();

    if (obj->cls != function_cls && obj->cls != builtin_function_or_method_cls && obj->cls != instancemethod_cls
        && obj->cls != classmethod_cls) {
        Box* rtn;
        if (rewrite_args) {
            // TODO is this ok?
            // rewrite_args->rewriter->trap();
            rtn = callattrInternal(obj, &call_str, CLASS_ONLY, rewrite_args, argspec, arg1, arg2, arg3, args,
                                   keyword_names);
        } else {
            rtn = callattrInternal(obj, &call_str, CLASS_ONLY, NULL, argspec, arg1, arg2, arg3, args, keyword_names);
        }
        if (!rtn)
            raiseExcHelper(TypeError, "'%s' object is not callable", getTypeName(obj));
        return rtn;
    }

    if (rewrite_args) {
        if (!rewrite_args->args_guarded) {
            // TODO should know which args don't need to be guarded, ex if we're guaranteed that they
            // already fit, either since the type inferencer could determine that,
            // or because they only need to fit into an UNKNOWN slot.

            if (npassed_args >= 1)
                rewrite_args->arg1->addAttrGuard(BOX_CLS_OFFSET, (intptr_t)arg1->cls);
            if (npassed_args >= 2)
                rewrite_args->arg2->addAttrGuard(BOX_CLS_OFFSET, (intptr_t)arg2->cls);
            if (npassed_args >= 3)
                rewrite_args->arg3->addAttrGuard(BOX_CLS_OFFSET, (intptr_t)arg3->cls);
            for (int i = 3; i < npassed_args; i++) {
                RewriterVar* v = rewrite_args->args->getAttr((i - 3) * sizeof(Box*), Location::any());
                v->addAttrGuard(BOX_CLS_OFFSET, (intptr_t)args[i - 3]->cls);
            }
            rewrite_args->args_guarded = true;
        }
    }

    if (obj->cls == function_cls || obj->cls == builtin_function_or_method_cls) {
        BoxedFunctionBase* f = static_cast<BoxedFunctionBase*>(obj);

        // Some functions are sufficiently important that we want them to be able to patchpoint themselves;
        // they can do this by setting the "internal_callable" field:
        CLFunction::InternalCallable callable = f->f->internal_callable;
        if (callable == NULL) {
            callable = callFunc;
        }
        Box* res = callable(f, rewrite_args, argspec, arg1, arg2, arg3, args, keyword_names);
        return res;
    } else if (obj->cls == classmethod_cls) {
        // TODO it's dumb but I should implement patchpoints here as well
        // duplicated with callattr
        BoxedClassmethod* cm = static_cast<BoxedClassmethod*>(obj);

        if (rewrite_args && !rewrite_args->func_guarded) {
            rewrite_args->obj->addAttrGuard(CLASSMETHOD_CALLABLE_OFFSET, (intptr_t)cm->cm_callable);
        }

        Box* rtn = runtimeCall(cm->cm_callable, argspec, arg1, arg2, arg3, args, keyword_names);
        return rtn;
    } else if (obj->cls == instancemethod_cls) {
        // TODO it's dumb but I should implement patchpoints here as well
        // duplicated with callattr
        BoxedInstanceMethod* im = static_cast<BoxedInstanceMethod*>(obj);

        if (rewrite_args && !rewrite_args->func_guarded) {
            rewrite_args->obj->addAttrGuard(INSTANCEMETHOD_FUNC_OFFSET, (intptr_t)im->func);
        }

        // Guard on which type of instancemethod (bound or unbound)
        // That is, if im->obj is NULL, guard on it being NULL
        // otherwise, guard on it being non-NULL
        if (rewrite_args) {
            rewrite_args->obj->addAttrGuard(INSTANCEMETHOD_OBJ_OFFSET, 0, im->obj != NULL);
        }

        // TODO guard on im->obj being NULL or not
        if (im->obj == NULL) {
            Box* f = im->func;
            if (rewrite_args) {
                rewrite_args->func_guarded = true;
                rewrite_args->args_guarded = true;
                rewrite_args->obj = rewrite_args->obj->getAttr(INSTANCEMETHOD_FUNC_OFFSET, Location::any());
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
                CallRewriteArgs srewrite_args(rewrite_args->rewriter, rewrite_args->obj, rewrite_args->destination);

                srewrite_args.arg1 = rewrite_args->obj->getAttr(INSTANCEMETHOD_OBJ_OFFSET, Location::any());
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
                    rewrite_args->out_rtn = srewrite_args.out_rtn;
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

        CallRewriteArgs rewrite_args(rewriter.get(), rewriter->getArg(0), rewriter->getReturnDestination());
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
            rewriter->commitReturning(rewrite_args.out_rtn);
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
        rewrite_args->lhs->addAttrGuard(BOX_CLS_OFFSET, (intptr_t)lhs->cls);
        rewrite_args->rhs->addAttrGuard(BOX_CLS_OFFSET, (intptr_t)rhs->cls);
    }

    Box* irtn = NULL;
    if (inplace) {
        std::string iop_name = getInplaceOpName(op_type);
        if (rewrite_args) {
            CallRewriteArgs srewrite_args(rewrite_args->rewriter, rewrite_args->lhs, rewrite_args->destination);
            srewrite_args.arg1 = rewrite_args->rhs;
            srewrite_args.args_guarded = true;
            irtn = callattrInternal1(lhs, &iop_name, CLASS_ONLY, &srewrite_args, ArgPassSpec(1), rhs);

            if (!srewrite_args.out_success) {
                rewrite_args = NULL;
            } else if (irtn) {
                if (irtn != NotImplemented)
                    rewrite_args->out_rtn = srewrite_args.out_rtn;
            }
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
        CallRewriteArgs srewrite_args(rewrite_args->rewriter, rewrite_args->lhs, rewrite_args->destination);
        srewrite_args.arg1 = rewrite_args->rhs;
        lrtn = callattrInternal1(lhs, &op_name, CLASS_ONLY, &srewrite_args, ArgPassSpec(1), rhs);

        if (!srewrite_args.out_success)
            rewrite_args = NULL;
        else if (lrtn) {
            if (lrtn != NotImplemented)
                rewrite_args->out_rtn = srewrite_args.out_rtn;
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
        REWRITE_ABORTED("");
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
                fprintf(stderr, "%s has %s, but returned NotImplemented\n", getTypeName(lhs), iop_name.c_str());
            else
                fprintf(stderr, "%s does not have %s\n", getTypeName(lhs), iop_name.c_str());
        }

        if (lrtn)
            fprintf(stderr, "%s has %s, but returned NotImplemented\n", getTypeName(lhs), op_name.c_str());
        else
            fprintf(stderr, "%s does not have %s\n", getTypeName(lhs), op_name.c_str());
        if (rrtn)
            fprintf(stderr, "%s has %s, but returned NotImplemented\n", getTypeName(rhs), rop_name.c_str());
        else
            fprintf(stderr, "%s does not have %s\n", getTypeName(rhs), rop_name.c_str());
    }

    raiseExcHelper(TypeError, "unsupported operand type(s) for %s%s: '%s' and '%s'", op_sym.data(), op_sym_suffix,
                   getTypeName(lhs), getTypeName(rhs));
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
                                      rewriter->getReturnDestination());
        rtn = binopInternal(lhs, rhs, op_type, false, &rewrite_args);
        assert(rtn);
        if (!rewrite_args.out_success) {
            rewriter.reset(NULL);
        } else
            rewriter->commitReturning(rewrite_args.out_rtn);
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
                                      rewriter->getReturnDestination());
        rtn = binopInternal(lhs, rhs, op_type, true, &rewrite_args);
        if (!rewrite_args.out_success) {
            rewriter.reset(NULL);
        } else {
            rewriter->commitReturning(rewrite_args.out_rtn);
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
            RewriterVar* cmpres = rewrite_args->lhs->cmp(neg ? AST_TYPE::NotEq : AST_TYPE::Eq, rewrite_args->rhs,
                                                         rewrite_args->destination);
            rewrite_args->out_rtn = rewrite_args->rewriter->call(false, (void*)boxBool, cmpres);
            rewrite_args->out_success = true;
        }

        return boxBool((lhs == rhs) ^ neg);
    }

    if (op_type == AST_TYPE::In || op_type == AST_TYPE::NotIn) {
        // TODO do rewrite

        Box* contained = callattrInternal1(rhs, &contains_str, CLASS_ONLY, NULL, ArgPassSpec(1), lhs);
        if (contained == NULL) {
            Box* iter = callattrInternal0(rhs, &iter_str, CLASS_ONLY, NULL, ArgPassSpec(0));
            if (iter)
                ASSERT(isUserDefined(rhs->cls), "%s should probably have a __contains__", getTypeName(rhs));
            RELEASE_ASSERT(iter == NULL, "need to try iterating");

            Box* getitem = typeLookup(rhs->cls, getitem_str, NULL);
            if (getitem)
                ASSERT(isUserDefined(rhs->cls), "%s should probably have a __contains__", getTypeName(rhs));
            RELEASE_ASSERT(getitem == NULL, "need to try old iteration protocol");

            raiseExcHelper(TypeError, "argument of type '%s' is not iterable", getTypeName(rhs));
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
        rewrite_args->lhs->addAttrGuard(BOX_CLS_OFFSET, (intptr_t)lhs->cls);
        rewrite_args->rhs->addAttrGuard(BOX_CLS_OFFSET, (intptr_t)rhs->cls);
    }

    const std::string& op_name = getOpName(op_type);
    Box* lrtn;
    if (rewrite_args) {
        CallRewriteArgs crewrite_args(rewrite_args->rewriter, rewrite_args->lhs, rewrite_args->destination);
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
            if (rewrite_args) {
                if (can_patchpoint) {
                    rewrite_args->out_success = true;
                }
            }
            return lrtn;
        }
    }

    const std::string& rop_name = getReverseOpName(op_type);
    Box* rrtn;
    if (rewrite_args) {
        CallRewriteArgs crewrite_args(rewrite_args->rewriter, rewrite_args->rhs, rewrite_args->destination);
        crewrite_args.arg1 = rewrite_args->lhs;
        rrtn = callattrInternal1(rhs, &rop_name, CLASS_ONLY, &crewrite_args, ArgPassSpec(1), lhs);

        if (!crewrite_args.out_success)
            rewrite_args = NULL;
        else if (rrtn)
            rewrite_args->out_rtn = crewrite_args.out_rtn;
    } else {
        rrtn = callattrInternal1(rhs, &rop_name, CLASS_ONLY, NULL, ArgPassSpec(1), lhs);
    }

    if (rrtn != NULL) {
        if (rrtn != NotImplemented) {
            bool can_patchpoint = !isUserDefined(lhs->cls) && !isUserDefined(rhs->cls);
            if (rewrite_args) {
                if (can_patchpoint) {
                    rewrite_args->out_success = true;
                }
            }
            return rrtn;
        }
    }

    // for cases where we don't have a __eq__/__neq__ method, just
    // embed the comparison as we do for Is/IsNot above
    if (!lrtn && !rrtn && rewrite_args) {
        assert(rewrite_args->out_success == false);
        if (op_type == AST_TYPE::Eq || AST_TYPE::NotEq) {
            rewrite_args->out_success = true;
            RewriterVar* cmpres
                = rewrite_args->lhs->cmp((AST_TYPE::AST_TYPE)op_type, rewrite_args->rhs, rewrite_args->destination);
            rewrite_args->out_rtn = rewrite_args->rewriter->call(false, (void*)boxBool, cmpres);
        } else {
            // TODO rewrite other compare ops
            rewrite_args = NULL;
            REWRITE_ABORTED("");
        }
    }

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
        CompareRewriteArgs rewrite_args(rewriter.get(), rewriter->getArg(0), rewriter->getArg(1),
                                        rewriter->getReturnDestination());
        rtn = compareInternal(lhs, rhs, op_type, &rewrite_args);
        if (!rewrite_args.out_success) {
            rewriter.reset(NULL);
        } else
            rewriter->commitReturning(rewrite_args.out_rtn);
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

    ASSERT(attr_func, "%s.%s", getTypeName(operand), op_name.c_str());

    Box* rtn = runtimeCall0(attr_func, ArgPassSpec(0));
    return rtn;
}

extern "C" Box* getitem(Box* value, Box* slice) {
    // This possibly could just be represented as a single callattr; the only tricky part
    // are the error messages.
    // Ex "(1)[1]" and "(1).__getitem__(1)" give different error messages.

    static StatCounter slowpath_getitem("slowpath_getitem");
    slowpath_getitem.log();

    std::unique_ptr<Rewriter> rewriter(
        Rewriter::createRewriter(__builtin_extract_return_addr(__builtin_return_address(0)), 2, "getitem"));

    Box* rtn;
    if (rewriter.get()) {
        CallRewriteArgs rewrite_args(rewriter.get(), rewriter->getArg(0), rewriter->getReturnDestination());
        rewrite_args.arg1 = rewriter->getArg(1);

        rtn = callattrInternal1(value, &getitem_str, CLASS_ONLY, &rewrite_args, ArgPassSpec(1), slice);

        if (!rewrite_args.out_success) {
            rewriter.reset(NULL);
        } else if (rtn)
            rewriter->commitReturning(rewrite_args.out_rtn);
    } else {
        rtn = callattrInternal1(value, &getitem_str, CLASS_ONLY, NULL, ArgPassSpec(1), slice);
    }

    if (rtn == NULL) {
        // different versions of python give different error messages for this:
        if (PYTHON_VERSION_MAJOR == 2 && PYTHON_VERSION_MINOR < 7) {
            raiseExcHelper(TypeError, "'%s' object is unsubscriptable", getTypeName(value)); // tested on 2.6.6
        } else if (PYTHON_VERSION_MAJOR == 2 && PYTHON_VERSION_MINOR == 7 && PYTHON_VERSION_MICRO < 3) {
            raiseExcHelper(TypeError, "'%s' object is not subscriptable", getTypeName(value)); // tested on 2.7.1
        } else {
            // Changed to this in 2.7.3:
            raiseExcHelper(TypeError, "'%s' object has no attribute '__getitem__'",
                           getTypeName(value)); // tested on 2.7.3
        }
    }

    return rtn;
}

// target[slice] = value
extern "C" void setitem(Box* target, Box* slice, Box* value) {
    static StatCounter slowpath_setitem("slowpath_setitem");
    slowpath_setitem.log();

    std::unique_ptr<Rewriter> rewriter(
        Rewriter::createRewriter(__builtin_extract_return_addr(__builtin_return_address(0)), 3, "setitem"));

    Box* rtn;
    if (rewriter.get()) {
        CallRewriteArgs rewrite_args(rewriter.get(), rewriter->getArg(0), rewriter->getReturnDestination());
        rewrite_args.arg1 = rewriter->getArg(1);
        rewrite_args.arg2 = rewriter->getArg(2);

        rtn = callattrInternal2(target, &setitem_str, CLASS_ONLY, &rewrite_args, ArgPassSpec(2), slice, value);

        if (!rewrite_args.out_success) {
            rewriter.reset(NULL);
        }
    } else {
        rtn = callattrInternal2(target, &setitem_str, CLASS_ONLY, NULL, ArgPassSpec(2), slice, value);
    }

    if (rtn == NULL) {
        raiseExcHelper(TypeError, "'%s' object does not support item assignment", getTypeName(target));
    }

    if (rewriter.get()) {
        rewriter->commit();
    }
}

// del target[start:end:step]
extern "C" void delitem(Box* target, Box* slice) {
    static StatCounter slowpath_delitem("slowpath_delitem");
    slowpath_delitem.log();

    std::unique_ptr<Rewriter> rewriter(
        Rewriter::createRewriter(__builtin_extract_return_addr(__builtin_return_address(0)), 2, "delitem"));

    Box* rtn;
    if (rewriter.get()) {
        CallRewriteArgs rewrite_args(rewriter.get(), rewriter->getArg(0), rewriter->getReturnDestination());
        rewrite_args.arg1 = rewriter->getArg(1);

        rtn = callattrInternal1(target, &delitem_str, CLASS_ONLY, &rewrite_args, ArgPassSpec(1), slice);

        if (!rewrite_args.out_success) {
            rewriter.reset(NULL);
        }

    } else {
        rtn = callattrInternal1(target, &delitem_str, CLASS_ONLY, NULL, ArgPassSpec(1), slice);
    }

    if (rtn == NULL) {
        raiseExcHelper(TypeError, "'%s' object does not support item deletion", getTypeName(target));
    }

    if (rewriter.get()) {
        rewriter->commit();
    }
}

void Box::delattr(const std::string& attr, DelattrRewriteArgs* rewrite_args) {
    if (cls->instancesHaveHCAttrs()) {
        // as soon as the hcls changes, the guard on hidden class won't pass.
        HCAttrs* attrs = getHCAttrsPtr();
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
        return;
    }

    if (cls->instancesHaveDictAttrs()) {
        Py_FatalError("unimplemented");
    }

    abort();
}

extern "C" void delattr_internal(Box* obj, const std::string& attr, bool allow_custom,
                                 DelattrRewriteArgs* rewrite_args) {
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
            raiseExcHelper(AttributeError, "'%s' object attribute '%s' is read-only", getTypeName(obj), attr.c_str());
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
            raiseExcHelper(TypeError, "can't set attributes of built-in/extension type '%s'\n", getNameOfClass(cobj));
        }
    }


    delattr_internal(obj, attr, true, NULL);
}

extern "C" Box* getPystonIter(Box* o) {
    Box* r = getiter(o);
    if (typeLookup(r->cls, hasnext_str, NULL) == NULL)
        return new BoxedIterWrapper(r);
    return r;
}

Box* getiter(Box* o) {
    static StatCounter slowpath_delattr("slowpath_getiter");
    slowpath_delattr.log();

    // TODO add rewriting to this?  probably want to try to avoid this path though
    Box* r = callattrInternal0(o, &iter_str, LookupScope::CLASS_ONLY, NULL, ArgPassSpec(0));
    if (r)
        return r;

    if (typeLookup(o->cls, getitem_str, NULL)) {
        return new BoxedSeqIter(o, 0);
    }

    raiseExcHelper(TypeError, "'%s' object is not iterable", getTypeName(o));
}

llvm::iterator_range<BoxIterator> Box::pyElements() {
    // TODO: this should probably call getPystonIter
    Box* iter = getiter(this);
    assert(iter);
    return llvm::iterator_range<BoxIterator>(++BoxIterator(iter), BoxIterator(nullptr));
}

// For use on __init__ return values
static void assertInitNone(Box* obj) {
    if (obj != None) {
        raiseExcHelper(TypeError, "__init__() should return None, not '%s'", getTypeName(obj));
    }
}

Box* typeNew(Box* _cls, Box* arg1, Box* arg2, Box** _args) {
    Box* arg3 = _args[0];

    if (!isSubclass(_cls->cls, type_cls))
        raiseExcHelper(TypeError, "type.__new__(X): X is not a type object (%s)", getTypeName(_cls));

    BoxedClass* cls = static_cast<BoxedClass*>(_cls);
    if (!isSubclass(cls, type_cls))
        raiseExcHelper(TypeError, "type.__new__(%s): %s is not a subtype of type", getNameOfClass(cls),
                       getNameOfClass(cls));

    if (arg2 == NULL) {
        assert(arg3 == NULL);
        BoxedClass* rtn = arg1->cls;
        return rtn;
    }

    RELEASE_ASSERT(arg3->cls == dict_cls, "%s", getTypeName(arg3));
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

    if ((base->tp_flags & Py_TPFLAGS_BASETYPE) == 0)
        raiseExcHelper(TypeError, "type '%.100s' is not an acceptable base type", base->tp_name);


    BoxedClass* made;

    if (base->instancesHaveDictAttrs() || base->instancesHaveHCAttrs()) {
        made = new (cls) BoxedHeapClass(base, NULL, base->attrs_offset, base->tp_basicsize, true, name);
    } else {
        assert(base->tp_basicsize % sizeof(void*) == 0);
        made = new (cls)
            BoxedHeapClass(base, NULL, base->tp_basicsize, base->tp_basicsize + sizeof(HCAttrs), true, name);
    }

    made->tp_dictoffset = base->tp_dictoffset;
    made->giveAttr("__module__", boxString(getCurrentModule()->name()));
    made->giveAttr("__doc__", None);

    for (const auto& p : attr_dict->d) {
        assert(p.first->cls == str_cls);
        made->setattr(static_cast<BoxedString*>(p.first)->s, p.second, NULL);
    }

    made->tp_new = base->tp_new;

    PystonType_Ready(made);
    fixup_slot_dispatchers(made);

    if (base->tp_alloc == &PystonType_GenericAlloc)
        made->tp_alloc = PystonType_GenericAlloc;
    else
        made->tp_alloc = PyType_GenericAlloc;

    return made;
}

Box* typeCallInternal(BoxedFunctionBase* f, CallRewriteArgs* rewrite_args, ArgPassSpec argspec, Box* arg1, Box* arg2,
                      Box* arg3, Box** args, const std::vector<const std::string*>* keyword_names) {
    int npassed_args = argspec.totalPassed();

    static StatCounter slowpath_typecall("slowpath_typecall");
    slowpath_typecall.log();

    if (argspec.has_starargs)
        return callFunc(f, rewrite_args, argspec, arg1, arg2, arg3, args, keyword_names);

    assert(argspec.num_args >= 1);
    Box* _cls = arg1;

    if (!isSubclass(_cls->cls, type_cls)) {
        raiseExcHelper(TypeError, "descriptor '__call__' requires a 'type' object but received an '%s'",
                       getTypeName(_cls));
    }

    BoxedClass* cls = static_cast<BoxedClass*>(_cls);

    RewriterVar* r_ccls = NULL;
    RewriterVar* r_new = NULL;
    RewriterVar* r_init = NULL;
    Box* new_attr, *init_attr;
    if (rewrite_args) {
        assert(!argspec.has_starargs);
        assert(argspec.num_args > 0);

        r_ccls = rewrite_args->arg1;
        // This is probably a duplicate, but it's hard to really convince myself of that.
        // Need to create a clear contract of who guards on what
        r_ccls->addGuard((intptr_t)arg1 /* = _cls */);
    }

    if (rewrite_args) {
        GetattrRewriteArgs grewrite_args(rewrite_args->rewriter, r_ccls, rewrite_args->destination);
        // TODO: if tp_new != Py_CallPythonNew, call that instead?
        new_attr = typeLookup(cls, new_str, &grewrite_args);

        if (!grewrite_args.out_success)
            rewrite_args = NULL;
        else {
            assert(new_attr);
            r_new = grewrite_args.out_rtn;
            r_new->addGuard((intptr_t)new_attr);
        }

        // Special-case functions to allow them to still rewrite:
        if (new_attr->cls != function_cls) {
            Box* descr_r = processDescriptorOrNull(new_attr, None, cls);
            if (descr_r) {
                new_attr = descr_r;
                rewrite_args = NULL;
                REWRITE_ABORTED("");
            }
        }
    } else {
        new_attr = typeLookup(cls, new_str, NULL);
        new_attr = processDescriptor(new_attr, None, cls);
    }
    assert(new_attr && "This should always resolve");

    // typeCall is tricky to rewrite since it has complicated behavior: we are supposed to
    // call the __init__ method of the *result of the __new__ call*, not of the original
    // class.  (And only if the result is an instance of the original class, but that's not
    // even the tricky part here.)
    //
    // By the time we know the type of the result of __new__(), it's too late to add traditional
    // guards.  So, instead of doing that, we're going to add a guard that makes sure that __new__
    // has the property that __new__(kls) always returns an instance of kls.
    //
    // Whitelist a set of __new__ methods that we know work like this.  Most importantly: object.__new__.
    //
    // Most builtin classes behave this way, but not all!
    // Notably, "type" itself does not.  For instance, assuming M is a subclass of
    // type, type.__new__(M, 1) will return the int class, which is not an instance of M.

    static Box* object_new = NULL;
    static Box* object_init = NULL;
    static std::vector<Box*> allowable_news;
    if (!object_new) {
        object_new = typeLookup(object_cls, new_str, NULL);
        assert(object_new);
        // I think this is unnecessary, but good form:
        gc::registerPermanentRoot(object_new);

        object_init = typeLookup(object_cls, init_str, NULL);
        assert(object_init);
        gc::registerPermanentRoot(object_init);

        allowable_news.push_back(object_new);

        for (BoxedClass* allowed_cls : { enumerate_cls, xrange_cls }) {
            auto new_obj = typeLookup(allowed_cls, new_str, NULL);
            gc::registerPermanentRoot(new_obj);
            allowable_news.push_back(new_obj);
        }
    }

    bool type_new_special_case;
    if (rewrite_args) {
        bool ok = false;
        for (auto b : allowable_news) {
            if (b == new_attr) {
                ok = true;
                break;
            }
        }

        if (!ok && (cls == int_cls || cls == float_cls || cls == long_cls)) {
            if (npassed_args == 1)
                ok = true;
            else if (npassed_args == 2 && (arg2->cls == int_cls || arg2->cls == str_cls || arg2->cls == float_cls))
                ok = true;
        }

        type_new_special_case = (cls == type_cls && argspec == ArgPassSpec(2));

        if (!ok && !type_new_special_case) {
            // Uncomment this to try to find __new__ functions that we could either white- or blacklist:
            // ASSERT(cls->is_user_defined || cls == type_cls, "Does '%s' have a well-behaved __new__?  if so, add to
            // allowable_news, otherwise add to the blacklist in this assert", cls->tp_name);
            rewrite_args = NULL;
            REWRITE_ABORTED("");
        }
    }

    if (rewrite_args) {
        GetattrRewriteArgs grewrite_args(rewrite_args->rewriter, r_ccls, rewrite_args->destination);
        init_attr = typeLookup(cls, init_str, &grewrite_args);

        if (!grewrite_args.out_success)
            rewrite_args = NULL;
        else {
            if (init_attr) {
                r_init = grewrite_args.out_rtn;
                r_init->addGuard((intptr_t)init_attr);
            }
        }
    } else {
        init_attr = typeLookup(cls, init_str, NULL);
    }
    // The init_attr should always resolve as well, but doesn't yet

    Box* made;
    RewriterVar* r_made = NULL;

    ArgPassSpec new_argspec = argspec;
    if (npassed_args > 1 && new_attr == object_new) {
        if (init_attr == object_init) {
            raiseExcHelper(TypeError, objectNewParameterTypeErrorMsg());
        } else {
            new_argspec = ArgPassSpec(1);
        }
    }

    if (rewrite_args) {
        if (new_attr == object_new && init_attr != object_init) {
            // Fast case: if we are calling object_new, we normally doesn't look at the arguments at all.
            // (Except in the case when init_attr != object_init, in which case object_new looks at the number
            // of arguments and throws an exception.)
            //
            // Another option is to rely on rewriting to make this fast, which would probably require adding
            // a custom internal callable to object.__new__
            made = objectNewNoArgs(cls);
            r_made = rewrite_args->rewriter->call(false, (void*)objectNewNoArgs, r_ccls);
        } else {
            CallRewriteArgs srewrite_args(rewrite_args->rewriter, r_new, rewrite_args->destination);
            srewrite_args.args_guarded = true;
            srewrite_args.func_guarded = true;

            int new_npassed_args = new_argspec.totalPassed();

            if (new_npassed_args >= 1)
                srewrite_args.arg1 = r_ccls;
            if (new_npassed_args >= 2)
                srewrite_args.arg2 = rewrite_args->arg2;
            if (new_npassed_args >= 3)
                srewrite_args.arg3 = rewrite_args->arg3;
            if (new_npassed_args >= 4)
                srewrite_args.args = rewrite_args->args;

            made = runtimeCallInternal(new_attr, &srewrite_args, new_argspec, cls, arg2, arg3, args, keyword_names);

            if (!srewrite_args.out_success) {
                rewrite_args = NULL;
            } else {
                r_made = srewrite_args.out_rtn;
            }
        }

        ASSERT(made->cls == cls || type_new_special_case,
               "We should only have allowed the rewrite to continue if we were guaranteed that made "
               "would have class cls!");
    } else {
        made = runtimeCallInternal(new_attr, NULL, new_argspec, cls, arg2, arg3, args, keyword_names);
    }

    assert(made);

    // Special-case (also a special case in CPython): if we just called type.__new__(arg), don't call __init__
    if (cls == type_cls && argspec == ArgPassSpec(2)) {
        if (rewrite_args) {
            rewrite_args->out_success = true;
            rewrite_args->out_rtn = r_made;
        }
        return made;
    }

    // If __new__ returns a subclass, supposed to call that subclass's __init__.
    // If __new__ returns a non-subclass, not supposed to call __init__.
    if (made->cls != cls) {
        ASSERT(rewrite_args == NULL, "We should only have allowed the rewrite to continue if we were guaranteed that "
                                     "made would have class cls!");

        if (!isSubclass(made->cls, cls)) {
            init_attr = NULL;
        } else {
            // We could have skipped the initial __init__ lookup
            init_attr = typeLookup(made->cls, init_str, NULL);
        }
    }

    if (init_attr && init_attr != object_init) {
        // TODO apply the same descriptor special-casing as in callattr?

        Box* initrtn;
        // Attempt to rewrite the basic case:
        if (rewrite_args && init_attr->cls == function_cls) {
            // Note: this code path includes the descriptor logic
            CallRewriteArgs srewrite_args(rewrite_args->rewriter, r_init, rewrite_args->destination);
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

            // initrtn = callattrInternal(cls, &_init_str, INST_ONLY, &srewrite_args, argspec, made, arg2, arg3, args,
            // keyword_names);
            initrtn = runtimeCallInternal(init_attr, &srewrite_args, argspec, made, arg2, arg3, args, keyword_names);

            if (!srewrite_args.out_success) {
                rewrite_args = NULL;
            } else {
                rewrite_args->rewriter->call(false, (void*)assertInitNone, srewrite_args.out_rtn);
            }
        } else {
            init_attr = processDescriptor(init_attr, made, cls);

            ArgPassSpec init_argspec = argspec;
            init_argspec.num_args--;

            int passed = init_argspec.totalPassed();

            // If we weren't passed the args array, it's not safe to index into it
            if (passed <= 2)
                initrtn = runtimeCallInternal(init_attr, NULL, init_argspec, arg2, arg3, NULL, NULL, keyword_names);
            else
                initrtn
                    = runtimeCallInternal(init_attr, NULL, init_argspec, arg2, arg3, args[0], &args[1], keyword_names);
        }
        assertInitNone(initrtn);
    } else {
        if (new_attr == NULL && npassed_args != 1) {
            // TODO not npassed args, since the starargs or kwargs could be null
            raiseExcHelper(TypeError, objectNewParameterTypeErrorMsg());
        }
    }

    if (rewrite_args) {
        rewrite_args->out_rtn = r_made;
        rewrite_args->out_success = true;
    }

    return made;
}

Box* typeCall(Box* obj, BoxedTuple* vararg, BoxedDict* kwargs) {
    assert(vararg->cls == tuple_cls);

    int n = vararg->elts.size();
    int args_to_pass = n + 2; // 1 for obj, 1 for kwargs

    Box** args = NULL;
    if (args_to_pass > 3)
        args = (Box**)alloca(sizeof(Box*) * (args_to_pass - 3));

    Box* arg1, *arg2, *arg3;
    arg1 = obj;
    for (int i = 0; i < n; i++) {
        getArg(i + 1, arg1, arg2, arg3, args) = vararg->elts[i];
    }
    getArg(n + 1, arg1, arg2, arg3, args) = kwargs;

    return typeCallInternal(NULL, NULL, ArgPassSpec(n + 1, 0, false, true), arg1, arg2, arg3, args, NULL);
}

extern "C" void delGlobal(BoxedModule* m, const std::string* name) {
    if (!m->getattr(*name)) {
        raiseExcHelper(NameError, "name '%s' is not defined", name->c_str());
    }
    m->delattr(*name, NULL);
}

extern "C" Box* getGlobal(BoxedModule* m, const std::string* name) {
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

            GetattrRewriteArgs rewrite_args(rewriter.get(), rewriter->getArg(0), rewriter->getReturnDestination());
            r = m->getattr(*name, &rewrite_args);
            if (!rewrite_args.out_success) {
                rewriter.reset(NULL);
            }
            if (r) {
                if (rewriter.get()) {
                    rewriter->commitReturning(rewrite_args.out_rtn);
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
                RewriterVar* r_rtn = rewriter->loadConst((intptr_t)builtins_module, rewriter->getReturnDestination());
                rewriter->commitReturning(r_rtn);
            }
            return builtins_module;
        }

        Box* rtn;
        if (rewriter.get()) {
            RewriterVar* builtins = rewriter->loadConst((intptr_t)builtins_module, Location::any());
            GetattrRewriteArgs rewrite_args(rewriter.get(), builtins, rewriter->getReturnDestination());
            rtn = builtins_module->getattr(*name, &rewrite_args);

            if (!rtn || !rewrite_args.out_success) {
                rewriter.reset(NULL);
            }

            if (rewriter.get()) {
                rewriter->commitReturning(rewrite_args.out_rtn);
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

extern "C" Box* importStar(Box* _from_module, BoxedModule* to_module) {
    assert(_from_module->cls == module_cls);
    BoxedModule* from_module = static_cast<BoxedModule*>(_from_module);

    Box* all = from_module->getattr(all_str);

    if (all) {
        Box* all_getitem = typeLookup(all->cls, getitem_str, NULL);
        if (!all_getitem)
            raiseExcHelper(TypeError, "'%s' object does not support indexing", getTypeName(all));

        int idx = 0;
        while (true) {
            Box* attr_name;
            try {
                attr_name = runtimeCallInternal2(all_getitem, NULL, ArgPassSpec(2), all, boxInt(idx));
            } catch (ExcInfo e) {
                if (e.matches(IndexError))
                    break;
                throw e;
            }
            idx++;

            if (attr_name->cls != str_cls)
                raiseExcHelper(TypeError, "attribute name must be string, not '%s'", getTypeName(attr_name));

            BoxedString* casted_attr_name = static_cast<BoxedString*>(attr_name);
            Box* attr_value = from_module->getattr(casted_attr_name->s);

            if (!attr_value)
                raiseExcHelper(AttributeError, "'module' object has no attribute '%s'", casted_attr_name->s.c_str());

            to_module->setattr(casted_attr_name->s, attr_value, NULL);
        }
        return None;
    }

    HCAttrs* module_attrs = from_module->getHCAttrsPtr();
    for (auto& p : module_attrs->hcls->attr_offsets) {
        if (p.first[0] == '_')
            continue;

        to_module->setattr(p.first, module_attrs->attr_list->attrs[p.second], NULL);
    }

    return None;
}
}
