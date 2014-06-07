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

#include "runtime/set.h"

#include <sstream>

#include "codegen/compvars.h"
#include "gc/collector.h"
#include "runtime/objmodel.h"

namespace pyston {

BoxedClass* set_cls, *set_iterator_cls;

extern "C" void setGCHandler(GCVisitor* v, void* p);
extern "C" void setIteratorGCHandler(GCVisitor* v, void* p);

const ObjectFlavor set_flavor(&setGCHandler, NULL);
const ObjectFlavor set_iterator_flavor(&setIteratorGCHandler, NULL);

namespace set {

class BoxedSetIterator : public Box {
public:
    BoxedSet* s;
    decltype(BoxedSet::s)::iterator it;

    BoxedSetIterator(BoxedSet* s) : Box(&set_iterator_flavor, set_iterator_cls), s(s), it(s->s.begin()) {}

    bool hasNext() { return it != s->s.end(); }

    Box* next() {
        Box* rtn = *it;
        ++it;
        return rtn;
    }
};

extern "C" void setGCHandler(GCVisitor* v, void* p) {
    boxGCHandler(v, p);

    BoxedSet* s = (BoxedSet*)p;

    // This feels like a cludge, but we need to find anything that
    // the unordered_map might have allocated.
    // Another way to handle this would be to rt_alloc the unordered_map
    // as well, though that incurs extra memory dereferences which would
    // be nice to avoid.
    void** start = (void**)&s->s;
    void** end = start + (sizeof(s->s) / 8);
    v->visitPotentialRange(start, end);
}

extern "C" void setIteratorGCHandler(GCVisitor* v, void* p) {
    boxGCHandler(v, p);

    BoxedSetIterator* it = (BoxedSetIterator*)p;

    v->visit(it->s);
}

Box* setiteratorHasnext(BoxedSetIterator* self) {
    assert(self->cls == set_iterator_cls);
    return boxBool(self->hasNext());
}

Box* setiteratorNext(BoxedSetIterator* self) {
    assert(self->cls == set_iterator_cls);
    return self->next();
}

Box* setAdd2(Box* _self, Box* b) {
    assert(_self->cls == set_cls);
    BoxedSet* self = static_cast<BoxedSet*>(_self);

    self->s.insert(b);
    return None;
}

Box* setNew(Box* cls, Box* container) {
    assert(cls == set_cls);

    if (container == None)
        return new BoxedSet();

    Box* rtn = new BoxedSet();
    for (Box* e : container->pyElements()) {
        setAdd2(rtn, e);
    }

    return rtn;
}

Box* setRepr(BoxedSet* self) {
    assert(self->cls == set_cls);

    std::ostringstream os("");

    os << "set([";
    bool first = true;
    for (Box* elt : self->s) {
        if (!first) {
            os << ", ";
        }
        os << static_cast<BoxedString*>(repr(elt))->s;
        first = false;
    }
    os << "])";
    return boxString(os.str());
}

Box* setOrSet(BoxedSet* lhs, BoxedSet* rhs) {
    assert(lhs->cls == set_cls);
    assert(rhs->cls == set_cls);

    BoxedSet* rtn = new BoxedSet();

    for (Box* elt : lhs->s) {
        rtn->s.insert(elt);
    }
    for (Box* elt : rhs->s) {
        rtn->s.insert(elt);
    }
    return rtn;
}

Box* setAndSet(BoxedSet* lhs, BoxedSet* rhs) {
    assert(lhs->cls == set_cls);
    assert(rhs->cls == set_cls);

    BoxedSet* rtn = new BoxedSet();

    for (Box* elt : lhs->s) {
        if (rhs->s.count(elt))
            rtn->s.insert(elt);
    }
    return rtn;
}

Box* setSubSet(BoxedSet* lhs, BoxedSet* rhs) {
    assert(lhs->cls == set_cls);
    assert(rhs->cls == set_cls);

    BoxedSet* rtn = new BoxedSet();

    for (Box* elt : lhs->s) {
        // TODO if len(rhs) << len(lhs), it might be more efficient
        // to delete the elements of rhs from lhs?
        if (rhs->s.count(elt) == 0)
            rtn->s.insert(elt);
    }
    return rtn;
}

Box* setXorSet(BoxedSet* lhs, BoxedSet* rhs) {
    assert(lhs->cls == set_cls);
    assert(rhs->cls == set_cls);

    BoxedSet* rtn = new BoxedSet();

    for (Box* elt : lhs->s) {
        if (rhs->s.count(elt) == 0)
            rtn->s.insert(elt);
    }

    for (Box* elt : rhs->s) {
        if (lhs->s.count(elt) == 0)
            rtn->s.insert(elt);
    }

    return rtn;
}

Box* setIter(BoxedSet* self) {
    assert(self->cls == set_cls);
    return new BoxedSetIterator(self);
}

Box* setLen(BoxedSet* self) {
    assert(self->cls == set_cls);
    return boxInt(self->s.size());
}

Box* setAdd(BoxedSet* self, Box* v) {
    assert(self->cls == set_cls);
    self->s.insert(v);
    return None;
}


} // namespace set

using namespace pyston::set;

void setupSet() {
    set_cls->giveAttr("__name__", boxStrConstant("set"));

    set_iterator_cls = new BoxedClass(object_cls, 0, sizeof(BoxedSet), false);
    set_iterator_cls->giveAttr("__name__", boxStrConstant("setiterator"));
    set_iterator_cls->giveAttr("__hasnext__",
                               new BoxedFunction(boxRTFunction((void*)setiteratorHasnext, BOXED_BOOL, 1)));
    set_iterator_cls->giveAttr("next", new BoxedFunction(boxRTFunction((void*)setiteratorNext, UNKNOWN, 1)));
    set_iterator_cls->freeze();
    gc::registerStaticRootObj(set_iterator_cls);

    set_cls->giveAttr("__new__",
                      new BoxedFunction(boxRTFunction((void*)setNew, UNKNOWN, 2, 1, false, false), { None }));

    Box* repr = new BoxedFunction(boxRTFunction((void*)setRepr, STR, 1));
    set_cls->giveAttr("__repr__", repr);
    set_cls->giveAttr("__str__", repr);

    std::vector<ConcreteCompilerType*> v_ss, v_su;
    v_ss.push_back(SET);
    v_ss.push_back(SET);
    v_su.push_back(SET);
    v_su.push_back(UNKNOWN);

    CLFunction* or_ = createRTFunction(2, 0, false, false);
    addRTFunction(or_, (void*)setOrSet, SET, v_ss);
    set_cls->giveAttr("__or__", new BoxedFunction(or_));

    CLFunction* sub_ = createRTFunction(2, 0, false, false);
    addRTFunction(sub_, (void*)setSubSet, SET, v_ss);
    set_cls->giveAttr("__sub__", new BoxedFunction(sub_));

    CLFunction* xor_ = createRTFunction(2, 0, false, false);
    addRTFunction(xor_, (void*)setXorSet, SET, v_ss);
    set_cls->giveAttr("__xor__", new BoxedFunction(xor_));

    CLFunction* and_ = createRTFunction(2, 0, false, false);
    addRTFunction(and_, (void*)setAndSet, SET, v_ss);
    set_cls->giveAttr("__and__", new BoxedFunction(and_));

    set_cls->giveAttr("__iter__", new BoxedFunction(boxRTFunction((void*)setIter, typeFromClass(set_iterator_cls), 1)));

    set_cls->giveAttr("__len__", new BoxedFunction(boxRTFunction((void*)setLen, BOXED_INT, 1)));

    set_cls->giveAttr("add", new BoxedFunction(boxRTFunction((void*)setAdd, NONE, 2)));

    set_cls->freeze();
}

void teardownSet() {
}
}
