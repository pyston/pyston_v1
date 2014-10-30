// Copyright (c) 2014 Dropbox, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.  // You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "runtime/types.h"

#include <cassert>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <stdint.h>

#include "core/options.h"
#include "core/stats.h"
#include "core/types.h"
#include "gc/collector.h"
#include "runtime/classobj.h"
#include "runtime/iterobject.h"
#include "runtime/long.h"
#include "runtime/objmodel.h"
#include "runtime/set.h"
#include "runtime/super.h"

extern "C" void initerrno();
extern "C" void init_sha();
extern "C" void init_sha256();
extern "C" void init_sha512();
extern "C" void init_md5();
extern "C" void init_random();
extern "C" void init_sre();
extern "C" void initmath();
extern "C" void initoperator();
extern "C" void initbinascii();

namespace pyston {

bool IN_SHUTDOWN = false;

#define SLICE_START_OFFSET ((char*)&(((BoxedSlice*)0x01)->start) - (char*)0x1)
#define SLICE_STOP_OFFSET ((char*)&(((BoxedSlice*)0x01)->stop) - (char*)0x1)
#define SLICE_STEP_OFFSET ((char*)&(((BoxedSlice*)0x01)->step) - (char*)0x1)

BoxIterator& BoxIterator::operator++() {
    static std::string hasnext_str("__hasnext__");
    static std::string next_str("next");

    Box* hasnext = callattrInternal(iter, &hasnext_str, CLASS_ONLY, NULL, ArgPassSpec(0), NULL, NULL, NULL, NULL, NULL);
    if (hasnext) {
        if (nonzero(hasnext)) {
            value = callattrInternal(iter, &next_str, CLASS_ONLY, NULL, ArgPassSpec(0), NULL, NULL, NULL, NULL, NULL);
        } else {
            iter = nullptr;
            value = nullptr;
        }
    } else {
        try {
            value = callattrInternal(iter, &next_str, CLASS_ONLY, NULL, ArgPassSpec(0), NULL, NULL, NULL, NULL, NULL);
        } catch (Box* e) {
            if ((e == StopIteration) || isSubclass(e->cls, StopIteration)) {
                iter = nullptr;
                value = nullptr;
            } else
                throw;
        }
    }
    return *this;
}

void BoxIterator::gcHandler(GCVisitor* v) {
    v->visitPotential(iter);
    v->visitPotential(value);
}

std::string builtinStr("__builtin__");

extern "C" BoxedFunction::BoxedFunction(CLFunction* f)
    : Box(function_cls), f(f), closure(NULL), isGenerator(false), ndefaults(0), defaults(NULL) {
    if (f->source) {
        assert(f->source->ast);
        // this->giveAttr("__name__", boxString(&f->source->ast->name));
        this->giveAttr("__name__", boxString(f->source->getName()));

        this->modname = f->source->parent_module->getattr("__name__", NULL);
    } else {
        this->modname = boxStringPtr(&builtinStr);
    }

    this->giveAttr("__doc__", None);

    assert(f->num_defaults == ndefaults);
}

extern "C" BoxedFunction::BoxedFunction(CLFunction* f, std::initializer_list<Box*> defaults, BoxedClosure* closure,
                                        bool isGenerator)
    : Box(function_cls), f(f), closure(closure), isGenerator(isGenerator), ndefaults(0), defaults(NULL) {
    if (defaults.size()) {
        // make sure to initialize defaults first, since the GC behavior is triggered by ndefaults,
        // and a GC can happen within this constructor:
        this->defaults = new (defaults.size()) GCdArray();
        memcpy(this->defaults->elts, defaults.begin(), defaults.size() * sizeof(Box*));
        this->ndefaults = defaults.size();
    }

    if (f->source) {
        assert(f->source->ast);
        // this->giveAttr("__name__", boxString(&f->source->ast->name));
        this->giveAttr("__name__", boxString(f->source->getName()));

        this->modname = f->source->parent_module->getattr("__name__", NULL);
    } else {
        this->modname = boxStringPtr(&builtinStr);
    }

    this->giveAttr("__doc__", None);

    assert(f->num_defaults == ndefaults);
}

// This probably belongs in dict.cpp?
extern "C" void functionGCHandler(GCVisitor* v, Box* b) {
    boxGCHandler(v, b);

    BoxedFunction* f = (BoxedFunction*)b;

    if (f->closure)
        v->visit(f->closure);

    // It's ok for f->defaults to be NULL here even if f->ndefaults isn't,
    // since we could be collecting from inside a BoxedFunction constructor
    if (f->ndefaults) {
        assert(f->defaults);
        v->visit(f->defaults);
        // do a conservative scan since there can be NULLs in there:
        v->visitPotentialRange(reinterpret_cast<void* const*>(&f->defaults->elts[0]),
                               reinterpret_cast<void* const*>(&f->defaults->elts[f->ndefaults]));
    }
}

BoxedModule::BoxedModule(const std::string& name, const std::string& fn) : Box(module_cls), fn(fn) {
    this->giveAttr("__name__", boxString(name));
    this->giveAttr("__file__", boxString(fn));
}

std::string BoxedModule::name() {
    Box* name = this->getattr("__name__");
    if (!name || name->cls != str_cls) {
        return "?";
    } else {
        BoxedString* sname = static_cast<BoxedString*>(name);
        return sname->s;
    }
}

extern "C" Box* boxCLFunction(CLFunction* f, BoxedClosure* closure, bool isGenerator,
                              std::initializer_list<Box*> defaults) {
    if (closure)
        assert(closure->cls == closure_cls);

    return new BoxedFunction(f, defaults, closure, isGenerator);
}

extern "C" CLFunction* unboxCLFunction(Box* b) {
    return static_cast<BoxedFunction*>(b)->f;
}

extern "C" void boxGCHandler(GCVisitor* v, Box* b) {
    if (b->cls) {
        v->visit(b->cls);

        if (b->cls->instancesHaveAttrs()) {
            HCAttrs* attrs = b->getAttrsPtr();

            v->visit(attrs->hcls);
            int nattrs = attrs->hcls->attr_offsets.size();
            if (nattrs) {
                HCAttrs::AttrList* attr_list = attrs->attr_list;
                assert(attr_list);
                v->visit(attr_list);
                v->visitRange((void**)&attr_list->attrs[0], (void**)&attr_list->attrs[nattrs]);
            }
        }
    } else {
        assert(type_cls == NULL || b == type_cls);
    }
}

extern "C" void typeGCHandler(GCVisitor* v, Box* b) {
    boxGCHandler(v, b);

    BoxedClass* cls = (BoxedClass*)b;

    if (cls->base)
        v->visit(cls->base);
}

extern "C" void instancemethodGCHandler(GCVisitor* v, Box* b) {
    BoxedInstanceMethod* im = (BoxedInstanceMethod*)b;

    if (im->obj) {
        v->visit(im->obj);
    }
    v->visit(im->func);
}

// This probably belongs in list.cpp?
extern "C" void listGCHandler(GCVisitor* v, Box* b) {
    boxGCHandler(v, b);

    BoxedList* l = (BoxedList*)b;
    int size = l->size;
    int capacity = l->capacity;
    assert(capacity >= size);
    if (capacity)
        v->visit(l->elts);
    if (size)
        v->visitRange((void**)&l->elts->elts[0], (void**)&l->elts->elts[size]);

    static StatCounter sc("gc_listelts_visited");
    sc.log(size);
}

extern "C" void setGCHandler(GCVisitor* v, Box* b) {
    boxGCHandler(v, b);

    BoxedSet* s = (BoxedSet*)b;

    // This feels like a cludge, but we need to find anything that
    // the unordered_map might have allocated.
    // Another way to handle this would be to rt_alloc the unordered_map
    // as well, though that incurs extra memory dereferences which would
    // be nice to avoid.
    void** start = (void**)&s->s;
    void** end = start + (sizeof(s->s) / 8);
    v->visitPotentialRange(start, end);
}

extern "C" void sliceGCHandler(GCVisitor* v, Box* b) {
    boxGCHandler(v, b);

    BoxedSlice* sl = static_cast<BoxedSlice*>(b);
    assert(sl->cls == slice_cls);

    v->visit(sl->start);
    v->visit(sl->stop);
    v->visit(sl->step);
}

// This probably belongs in tuple.cpp?
extern "C" void tupleGCHandler(GCVisitor* v, Box* b) {
    boxGCHandler(v, b);

    BoxedTuple* t = (BoxedTuple*)b;
    v->visitPotentialRange((void* const*)&t->elts, (void* const*)(&t->elts + 1));
}

// This probably belongs in dict.cpp?
extern "C" void dictGCHandler(GCVisitor* v, Box* b) {
    boxGCHandler(v, b);

    BoxedDict* d = (BoxedDict*)b;

    // This feels like a cludge, but we need to find anything that
    // the unordered_map might have allocated.
    // Another way to handle this would be to rt_alloc the unordered_map
    // as well, though that incurs extra memory dereferences which would
    // be nice to avoid.
    void** start = (void**)&d->d;
    void** end = start + (sizeof(d->d) / 8);
    v->visitPotentialRange(start, end);
}

extern "C" void closureGCHandler(GCVisitor* v, Box* b) {
    boxGCHandler(v, b);

    BoxedClosure* c = (BoxedClosure*)b;
    if (c->parent)
        v->visit(c->parent);
}

extern "C" {
BoxedClass* object_cls, *type_cls, *none_cls, *bool_cls, *int_cls, *float_cls, *str_cls, *function_cls,
    *instancemethod_cls, *list_cls, *slice_cls, *module_cls, *dict_cls, *tuple_cls, *file_cls, *member_cls,
    *closure_cls, *generator_cls, *complex_cls, *basestring_cls, *unicode_cls, *property_cls;


BoxedTuple* EmptyTuple;
}

extern "C" Box* createUserClass(std::string* name, Box* _bases, Box* _attr_dict) {
    ASSERT(_attr_dict->cls == dict_cls, "%s", getTypeName(_attr_dict)->c_str());
    BoxedDict* attr_dict = static_cast<BoxedDict*>(_attr_dict);

    assert(_bases->cls == tuple_cls);
    BoxedTuple* bases = static_cast<BoxedTuple*>(_bases);

    Box* metaclass = NULL;
    metaclass = attr_dict->getOrNull(boxStrConstant("__metaclass__"));

    if (metaclass != NULL) {
    } else if (bases->elts.size() > 0) {
        // TODO Apparently this is supposed to look up __class__, and if that throws
        // an error, then look up ob_type (aka cls)
        metaclass = bases->elts[0]->cls;
    } else {
        BoxedModule* m = getCurrentModule();
        metaclass = m->getattr("__metaclass__");

        if (!metaclass) {
            metaclass = classobj_cls;
        }
    }
    assert(metaclass);

    Box* r = runtimeCall(metaclass, ArgPassSpec(3), boxStringPtr(name), _bases, _attr_dict, NULL, NULL);
    RELEASE_ASSERT(r, "");
    return r;
}

extern "C" Box* boxInstanceMethod(Box* obj, Box* func) {
    static StatCounter num_ims("num_instancemethods");
    num_ims.log();

    return new BoxedInstanceMethod(obj, func);
}

extern "C" Box* boxUnboundInstanceMethod(Box* func) {
    return new BoxedInstanceMethod(NULL, func);
}

extern "C" BoxedString* noneRepr(Box* v) {
    return new BoxedString("None");
}

extern "C" Box* noneHash(Box* v) {
    return boxInt(819239); // chosen randomly
}

extern "C" Box* noneNonzero(Box* v) {
    return False;
}

extern "C" BoxedString* functionRepr(BoxedFunction* v) {
    // TODO there has to be a better way
    if (v == repr_obj)
        return boxStrConstant("<built-in function repr>");
    if (v == len_obj)
        return boxStrConstant("<built-in function len>");
    if (v == hash_obj)
        return boxStrConstant("<built-in function hash>");
    if (v == range_obj)
        return boxStrConstant("<built-in function range>");
    if (v == abs_obj)
        return boxStrConstant("<built-in function abs>");
    if (v == min_obj)
        return boxStrConstant("<built-in function min>");
    if (v == max_obj)
        return boxStrConstant("<built-in function max>");
    if (v == open_obj)
        return boxStrConstant("<built-in function open>");
    if (v == id_obj)
        return boxStrConstant("<built-in function id>");
    if (v == chr_obj)
        return boxStrConstant("<built-in function chr>");
    if (v == ord_obj)
        return boxStrConstant("<built-in function ord>");
    return new BoxedString("function");
}

static Box* functionGet(BoxedFunction* self, Box* inst, Box* owner) {
    RELEASE_ASSERT(self->cls == function_cls, "");

    if (inst == None)
        return boxUnboundInstanceMethod(self);
    return boxInstanceMethod(inst, self);
}

static Box* functionCall(BoxedFunction* self, Box* args, Box* kwargs) {
    RELEASE_ASSERT(self->cls == function_cls, "%s", getTypeName(self)->c_str());

    // This won't work if you subclass from function_cls, since runtimeCall will
    // just call back into this function.
    // Fortunately, CPython disallows subclassing FunctionType; we don't currently
    // disallow it but it's good to know.

    assert(args->cls == tuple_cls);
    assert(kwargs->cls == dict_cls);
    return runtimeCall(self, ArgPassSpec(0, 0, true, true), args, kwargs, NULL, NULL, NULL);
}

extern "C" {
Box* None = NULL;
Box* NotImplemented = NULL;
Box* repr_obj = NULL;
Box* len_obj = NULL;
Box* hash_obj = NULL;
Box* abs_obj = NULL;
Box* min_obj = NULL;
Box* max_obj = NULL;
Box* open_obj = NULL;
Box* id_obj = NULL;
Box* chr_obj = NULL;
Box* ord_obj = NULL;
Box* trap_obj = NULL;
Box* range_obj = NULL;
}

HiddenClass* root_hcls;

extern "C" Box* createSlice(Box* start, Box* stop, Box* step) {
    BoxedSlice* rtn = new BoxedSlice(start, stop, step);
    return rtn;
}

extern "C" BoxedClosure* createClosure(BoxedClosure* parent_closure) {
    if (parent_closure)
        assert(parent_closure->cls == closure_cls);
    return new BoxedClosure(parent_closure);
}

extern "C" Box* sliceNew(Box* cls, Box* start, Box* stop, Box** args) {
    RELEASE_ASSERT(cls == slice_cls, "");
    Box* step = args[0];

    if (stop == NULL)
        return createSlice(None, start, None);
    return createSlice(start, stop, step);
}

Box* instancemethodRepr(BoxedInstanceMethod* self) {
    if (self->obj)
        return boxStrConstant("<bound instancemethod object>");
    else
        return boxStrConstant("<unbound instancemethod object>");
}

Box* instancemethodEq(BoxedInstanceMethod* self, Box* rhs) {
    if (rhs->cls != instancemethod_cls) {
        return boxBool(false);
    }

    BoxedInstanceMethod* rhs_im = static_cast<BoxedInstanceMethod*>(rhs);
    if (self->func == rhs_im->func) {
        if (self->obj == NULL && rhs_im->obj == NULL) {
            return boxBool(true);
        } else {
            if (self->obj != NULL && rhs_im->obj != NULL) {
                return compareInternal(self->obj, rhs_im->obj, AST_TYPE::Eq, NULL);
            } else {
                return boxBool(false);
            }
        }
    } else {
        return boxBool(false);
    }
}

Box* sliceRepr(BoxedSlice* self) {
    BoxedString* start = static_cast<BoxedString*>(repr(self->start));
    BoxedString* stop = static_cast<BoxedString*>(repr(self->stop));
    BoxedString* step = static_cast<BoxedString*>(repr(self->step));
    std::string s = "slice(" + start->s + ", " + stop->s + ", " + step->s + ")";
    return new BoxedString(s);
}

Box* typeRepr(BoxedClass* self) {
    if (isUserDefined(self)) {
        std::ostringstream os;
        os << "<class '";

        Box* m = self->getattr("__module__");
        RELEASE_ASSERT(m, "");
        if (m->cls == str_cls) {
            BoxedString* sm = static_cast<BoxedString*>(m);
            os << sm->s << '.';
        }

        Box* n = self->getattr("__name__");
        RELEASE_ASSERT(n, "");
        RELEASE_ASSERT(n->cls == str_cls, "should have prevented you from setting __name__ to non-string");
        BoxedString* sn = static_cast<BoxedString*>(n);
        os << sn->s;

        os << "'>";

        return boxString(os.str());
    } else {
        char buf[80];
        snprintf(buf, 80, "<type '%s'>", getNameOfClass(self)->c_str());
        return boxStrConstant(buf);
    }
}

Box* typeHash(BoxedClass* self) {
    assert(isSubclass(self->cls, type_cls));

    // This is how CPython defines it; seems reasonable enough:
    return boxInt(reinterpret_cast<intptr_t>(self) >> 4);
}

Box* moduleRepr(BoxedModule* m) {
    assert(m->cls == module_cls);

    std::ostringstream os;
    os << "<module '" << m->name() << "' ";

    if (m->fn == "__builtin__") {
        os << "(built-in)>";
    } else {
        os << "from '" << m->fn << "'>";
    }
    return boxString(os.str());
}

CLFunction* unboxRTFunction(Box* b) {
    assert(b->cls == function_cls);
    return static_cast<BoxedFunction*>(b)->f;
}

// A dictionary-like wrapper around the attributes array.
// Not sure if this will be enough to satisfy users who expect __dict__
// or PyModule_GetDict to return real dicts.
BoxedClass* attrwrapper_cls;
class AttrWrapper : public Box {
private:
    Box* b;

public:
    AttrWrapper(Box* b) : Box(attrwrapper_cls), b(b) {}

    static void gcHandler(GCVisitor* v, Box* b) {
        boxGCHandler(v, b);

        AttrWrapper* aw = (AttrWrapper*)b;
        v->visit(aw->b);
    }

    static Box* setitem(Box* _self, Box* _key, Box* value) {
        RELEASE_ASSERT(_self->cls == attrwrapper_cls, "");
        AttrWrapper* self = static_cast<AttrWrapper*>(_self);

        RELEASE_ASSERT(_key->cls == str_cls, "");
        BoxedString* key = static_cast<BoxedString*>(_key);
        self->b->setattr(key->s, value, NULL);
        return None;
    }

    static Box* getitem(Box* _self, Box* _key) {
        RELEASE_ASSERT(_self->cls == attrwrapper_cls, "");
        AttrWrapper* self = static_cast<AttrWrapper*>(_self);

        RELEASE_ASSERT(_key->cls == str_cls, "");
        BoxedString* key = static_cast<BoxedString*>(_key);
        Box* r = self->b->getattr(key->s);
        if (!r) {
            raiseExcHelper(KeyError, "'%s'", key->s.c_str());
        }
        return r;
    }

    static Box* str(Box* _self) {
        RELEASE_ASSERT(_self->cls == attrwrapper_cls, "");
        AttrWrapper* self = static_cast<AttrWrapper*>(_self);

        std::ostringstream os("");
        os << "attrwrapper({";

        HCAttrs* attrs = self->b->getAttrsPtr();
        bool first = true;
        for (const auto& p : attrs->hcls->attr_offsets) {
            if (!first)
                os << ", ";
            first = false;

            BoxedString* v = repr(attrs->attr_list->attrs[p.second]);
            os << p.first << ": " << v->s;
        }
        os << "})";
        return boxString(os.str());
    }

    static Box* contains(Box* _self, Box* _key) {
        RELEASE_ASSERT(_self->cls == attrwrapper_cls, "");
        AttrWrapper* self = static_cast<AttrWrapper*>(_self);

        RELEASE_ASSERT(_key->cls == str_cls, "");
        BoxedString* key = static_cast<BoxedString*>(_key);
        Box* r = self->b->getattr(key->s);
        return r ? True : False;
    }
};

Box* makeAttrWrapper(Box* b) {
    assert(b->cls->instancesHaveAttrs());
    return new AttrWrapper(b);
}

Box* objectNew(BoxedClass* cls, BoxedTuple* args) {
    assert(isSubclass(cls->cls, type_cls));
    assert(args->cls == tuple_cls);

    if (args->elts.size() != 0) {
        // TODO slow
        if (typeLookup(cls, "__init__", NULL) == typeLookup(object_cls, "__init__", NULL))
            raiseExcHelper(TypeError, objectNewParameterTypeErrorMsg());
    }

    assert(cls->tp_basicsize >= sizeof(Box));
    void* mem = gc::gc_alloc(cls->tp_basicsize, gc::GCKind::PYTHON);

    Box* rtn = ::new (mem) Box(cls);
    initUserAttrs(rtn, cls);
    return rtn;
}

Box* objectInit(Box* b, BoxedTuple* args) {
    return None;
}

Box* objectRepr(Box* obj) {
    char buf[80];
    if (obj->cls == type_cls) {
        snprintf(buf, 80, "<type '%s'>", getNameOfClass(static_cast<BoxedClass*>(obj))->c_str());
    } else {
        snprintf(buf, 80, "<%s object at %p>", getTypeName(obj)->c_str(), obj);
    }
    return boxStrConstant(buf);
}

Box* objectStr(Box* obj) {
    static const std::string repr_str("__repr__");
    return callattrInternal(obj, &repr_str, CLASS_ONLY, NULL, ArgPassSpec(0), NULL, NULL, NULL, NULL, NULL);
}

bool TRACK_ALLOCATIONS = false;
void setupRuntime() {
    root_hcls = HiddenClass::makeRoot();
    gc::registerPermanentRoot(root_hcls);

    object_cls = new BoxedClass(NULL, NULL, &boxGCHandler, 0, sizeof(Box), false);
    type_cls = new BoxedClass(NULL, object_cls, &typeGCHandler, offsetof(BoxedClass, attrs), sizeof(BoxedClass), false);
    type_cls->cls = type_cls;
    object_cls->cls = type_cls;

    none_cls = new BoxedClass(type_cls, object_cls, NULL, 0, sizeof(Box), false);
    None = new Box(none_cls);
    gc::registerPermanentRoot(None);

    // You can't actually have an instance of basestring
    basestring_cls = new BoxedClass(type_cls, object_cls, NULL, 0, sizeof(Box), false);

    // TODO we leak all the string data!
    str_cls = new BoxedClass(type_cls, basestring_cls, NULL, 0, sizeof(BoxedString), false);
    unicode_cls = new BoxedClass(type_cls, basestring_cls, NULL, 0, sizeof(BoxedUnicode), false);

    // It wasn't safe to add __base__ attributes until object+type+str are set up, so do that now:
    type_cls->giveAttr("__base__", object_cls);
    basestring_cls->giveAttr("__base__", object_cls);
    str_cls->giveAttr("__base__", basestring_cls);
    none_cls->giveAttr("__base__", object_cls);
    object_cls->giveAttr("__base__", None);


    tuple_cls = new BoxedClass(type_cls, object_cls, &tupleGCHandler, 0, sizeof(BoxedTuple), false);
    EmptyTuple = new BoxedTuple({});
    gc::registerPermanentRoot(EmptyTuple);


    module_cls = new BoxedClass(type_cls, object_cls, NULL, offsetof(BoxedModule, attrs), sizeof(BoxedModule), false);

    // TODO it'd be nice to be able to do these in the respective setupType methods,
    // but those setup methods probably want access to these objects.
    // We could have a multi-stage setup process, but that seems overkill for now.
    bool_cls = new BoxedClass(type_cls, object_cls, NULL, 0, sizeof(BoxedBool), false);
    int_cls = new BoxedClass(type_cls, object_cls, NULL, 0, sizeof(BoxedInt), false);
    complex_cls = new BoxedClass(type_cls, object_cls, NULL, 0, sizeof(BoxedComplex), false);
    // TODO we're leaking long memory!
    long_cls = new BoxedClass(type_cls, object_cls, NULL, 0, sizeof(BoxedLong), false);
    float_cls = new BoxedClass(type_cls, object_cls, NULL, 0, sizeof(BoxedFloat), false);
    function_cls = new BoxedClass(type_cls, object_cls, &functionGCHandler, offsetof(BoxedFunction, attrs),
                                  sizeof(BoxedFunction), false);
    instancemethod_cls
        = new BoxedClass(type_cls, object_cls, &instancemethodGCHandler, 0, sizeof(BoxedInstanceMethod), false);
    list_cls = new BoxedClass(type_cls, object_cls, &listGCHandler, 0, sizeof(BoxedList), false);
    slice_cls = new BoxedClass(type_cls, object_cls, &sliceGCHandler, 0, sizeof(BoxedSlice), false);
    dict_cls = new BoxedClass(type_cls, object_cls, &dictGCHandler, 0, sizeof(BoxedDict), false);
    file_cls = new BoxedClass(type_cls, object_cls, NULL, 0, sizeof(BoxedFile), false);
    set_cls = new BoxedClass(type_cls, object_cls, &setGCHandler, 0, sizeof(BoxedSet), false);
    frozenset_cls = new BoxedClass(type_cls, object_cls, &setGCHandler, 0, sizeof(BoxedSet), false);
    member_cls = new BoxedClass(type_cls, object_cls, NULL, 0, sizeof(BoxedMemberDescriptor), false);
    closure_cls = new BoxedClass(type_cls, object_cls, &closureGCHandler, offsetof(BoxedClosure, attrs),
                                 sizeof(BoxedClosure), false);
    property_cls = new BoxedClass(type_cls, object_cls, NULL, 0, sizeof(BoxedProperty), false);
    attrwrapper_cls = new BoxedClass(type_cls, object_cls, &AttrWrapper::gcHandler, 0, sizeof(AttrWrapper), false);

    STR = typeFromClass(str_cls);
    BOXED_INT = typeFromClass(int_cls);
    BOXED_FLOAT = typeFromClass(float_cls);
    BOXED_BOOL = typeFromClass(bool_cls);
    NONE = typeFromClass(none_cls);
    LIST = typeFromClass(list_cls);
    SLICE = typeFromClass(slice_cls);
    MODULE = typeFromClass(module_cls);
    DICT = typeFromClass(dict_cls);
    SET = typeFromClass(set_cls);
    FROZENSET = typeFromClass(frozenset_cls);
    BOXED_TUPLE = typeFromClass(tuple_cls);
    LONG = typeFromClass(long_cls);
    BOXED_COMPLEX = typeFromClass(complex_cls);

    object_cls->giveAttr("__name__", boxStrConstant("object"));
    object_cls->giveAttr("__new__", new BoxedFunction(boxRTFunction((void*)objectNew, UNKNOWN, 1, 0, true, false)));
    object_cls->giveAttr("__init__", new BoxedFunction(boxRTFunction((void*)objectInit, UNKNOWN, 1, 0, true, false)));
    object_cls->giveAttr("__repr__", new BoxedFunction(boxRTFunction((void*)objectRepr, UNKNOWN, 1, 0, false, false)));
    object_cls->giveAttr("__str__", new BoxedFunction(boxRTFunction((void*)objectStr, UNKNOWN, 1, 0, false, false)));
    object_cls->freeze();

    auto typeCallObj = boxRTFunction((void*)typeCall, UNKNOWN, 1, 0, true, false);
    typeCallObj->internal_callable = &typeCallInternal;
    type_cls->giveAttr("__call__", new BoxedFunction(typeCallObj));

    type_cls->giveAttr("__name__", boxStrConstant("type"));
    type_cls->giveAttr("__new__",
                       new BoxedFunction(boxRTFunction((void*)typeNew, UNKNOWN, 4, 2, false, false), { NULL, NULL }));
    type_cls->giveAttr("__repr__", new BoxedFunction(boxRTFunction((void*)typeRepr, STR, 1)));
    type_cls->giveAttr("__str__", type_cls->getattr("__repr__"));
    type_cls->giveAttr("__hash__", new BoxedFunction(boxRTFunction((void*)typeHash, BOXED_INT, 1)));
    type_cls->freeze();

    none_cls->giveAttr("__name__", boxStrConstant("NoneType"));
    none_cls->giveAttr("__repr__", new BoxedFunction(boxRTFunction((void*)noneRepr, STR, 1)));
    none_cls->giveAttr("__str__", none_cls->getattr("__repr__"));
    none_cls->giveAttr("__hash__", new BoxedFunction(boxRTFunction((void*)noneHash, UNKNOWN, 1)));
    none_cls->giveAttr("__nonzero__", new BoxedFunction(boxRTFunction((void*)noneNonzero, BOXED_BOOL, 1)));
    none_cls->freeze();

    module_cls->giveAttr("__name__", boxStrConstant("module"));
    module_cls->giveAttr("__repr__", new BoxedFunction(boxRTFunction((void*)moduleRepr, STR, 1)));
    module_cls->giveAttr("__str__", module_cls->getattr("__repr__"));
    module_cls->freeze();

    closure_cls->giveAttr("__name__", boxStrConstant("closure"));
    closure_cls->freeze();

    setupBool();
    setupInt();
    setupLong();
    setupFloat();
    setupComplex();
    setupStr();
    setupList();
    setupDict();
    setupSet();
    setupTuple();
    setupFile();
    setupGenerator();
    setupIter();
    setupClassobj();
    setupSuper();
    setupUnicode();
    setupDescr();

    function_cls->giveAttr("__name__", boxStrConstant("function"));
    function_cls->giveAttr("__repr__", new BoxedFunction(boxRTFunction((void*)functionRepr, STR, 1)));
    function_cls->giveAttr("__str__", function_cls->getattr("__repr__"));
    function_cls->giveAttr("__module__",
                           new BoxedMemberDescriptor(BoxedMemberDescriptor::OBJECT, offsetof(BoxedFunction, modname)));
    function_cls->giveAttr("__get__", new BoxedFunction(boxRTFunction((void*)functionGet, UNKNOWN, 3)));
    function_cls->giveAttr("__call__",
                           new BoxedFunction(boxRTFunction((void*)functionCall, UNKNOWN, 1, 0, true, true)));
    function_cls->freeze();

    instancemethod_cls->giveAttr("__name__", boxStrConstant("instancemethod"));
    instancemethod_cls->giveAttr("__repr__", new BoxedFunction(boxRTFunction((void*)instancemethodRepr, STR, 1)));
    instancemethod_cls->giveAttr("__eq__", new BoxedFunction(boxRTFunction((void*)instancemethodEq, UNKNOWN, 2)));
    instancemethod_cls->freeze();

    slice_cls->giveAttr("__name__", boxStrConstant("slice"));
    slice_cls->giveAttr("__new__",
                        new BoxedFunction(boxRTFunction((void*)sliceNew, UNKNOWN, 4, 2, false, false), { NULL, None }));
    slice_cls->giveAttr("__repr__", new BoxedFunction(boxRTFunction((void*)sliceRepr, STR, 1)));
    slice_cls->giveAttr("__str__", slice_cls->getattr("__repr__"));
    slice_cls->giveAttr("start", new BoxedMemberDescriptor(BoxedMemberDescriptor::OBJECT, SLICE_START_OFFSET));
    slice_cls->giveAttr("stop", new BoxedMemberDescriptor(BoxedMemberDescriptor::OBJECT, SLICE_STOP_OFFSET));
    slice_cls->giveAttr("step", new BoxedMemberDescriptor(BoxedMemberDescriptor::OBJECT, SLICE_STEP_OFFSET));
    slice_cls->freeze();

    attrwrapper_cls->giveAttr("__name__", boxStrConstant("attrwrapper"));
    attrwrapper_cls->giveAttr("__setitem__", new BoxedFunction(boxRTFunction((void*)AttrWrapper::setitem, UNKNOWN, 3)));
    attrwrapper_cls->giveAttr("__getitem__", new BoxedFunction(boxRTFunction((void*)AttrWrapper::getitem, UNKNOWN, 2)));
    attrwrapper_cls->giveAttr("__str__", new BoxedFunction(boxRTFunction((void*)AttrWrapper::str, UNKNOWN, 1)));
    attrwrapper_cls->giveAttr("__contains__",
                              new BoxedFunction(boxRTFunction((void*)AttrWrapper::contains, UNKNOWN, 2)));
    attrwrapper_cls->freeze();

    // sys is the first module that needs to be set up, due to modules
    // being tracked in sys.modules:
    setupSys();

    setupBuiltins();
    setupTime();
    setupThread();
    setupPosix();

    setupCAPI();

    initerrno();
    init_sha();
    init_sha256();
    init_sha512();
    init_md5();
    init_random();
    init_sre();
    initmath();
    initoperator();
    initbinascii();

    setupSysEnd();

    Stats::endOfInit();

    TRACK_ALLOCATIONS = true;
}

BoxedModule* createModule(const std::string& name, const std::string& fn) {
    assert(fn.size() && "probably wanted to set the fn to <stdin>?");
    BoxedModule* module = new BoxedModule(name, fn);

    BoxedDict* d = getSysModulesDict();
    Box* b_name = boxStringPtr(&name);
    assert(d->d.count(b_name) == 0);
    d->d[b_name] = module;

    module->giveAttr("__doc__", None);
    return module;
}

void freeHiddenClasses(HiddenClass* hcls) {
    for (const auto& p : hcls->children) {
        freeHiddenClasses(p.second);
    }
    gc::gc_free(hcls);
}

void teardownRuntime() {
    // Things start to become very precarious after this point, as the basic classes stop to work.
    // TODO it's probably a waste of time to tear these down in non-debugging mode
    IN_SHUTDOWN = true;

    if (VERBOSITY("runtime") >= 1)
        printf("In teardownRuntime\n");

    teardownCAPI();

    teardownList();
    teardownInt();
    teardownFloat();
    teardownComplex();
    teardownStr();
    teardownBool();
    teardownDict();
    teardownSet();
    teardownTuple();
    teardownFile();
    teardownDescr();

    /*
    // clear all the attributes on the base classes before freeing the classes themselves,
    // since we will run into problem if we free a class but there is an object somewhere
    // else that refers to it.
    clearAttrs(bool_cls);
    clearAttrs(int_cls);
    clearAttrs(float_cls);
    clearAttrs(none_cls);
    clearAttrs(function_cls);
    clearAttrs(instancemethod_cls);
    clearAttrs(str_cls);
    clearAttrs(list_cls);
    clearAttrs(slice_cls);
    clearAttrs(type_cls);
    clearAttrs(module_cls);
    clearAttrs(dict_cls);
    clearAttrs(tuple_cls);
    clearAttrs(file_cls);

    decref(bool_cls);
    decref(int_cls);
    decref(float_cls);
    decref(function_cls);
    decref(instancemethod_cls);
    decref(str_cls);
    decref(list_cls);
    decref(slice_cls);
    decref(module_cls);
    decref(dict_cls);
    decref(tuple_cls);
    decref(file_cls);

    ASSERT(None->nrefs == 1, "%ld", None->nrefs);
    decref(None);

    decref(none_cls);
    decref(type_cls);
    */

    freeHiddenClasses(root_hcls);
}
}
