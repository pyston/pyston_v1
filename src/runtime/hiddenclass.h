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

#ifndef PYSTON_RUNTIME_HIDDENCLASS_H
#define PYSTON_RUNTIME_HIDDENCLASS_H

#include <llvm/ADT/StringMap.h>

#include "Python.h"

#include "core/contiguous_map.h"
#include "core/types.h"
#include "gc/gc_alloc.h"

namespace pyston {

class HiddenClass : public GCAllocated<gc::GCKind::HIDDEN_CLASS> {
public:
    // We have a couple different storage strategies for attributes, which
    // are distinguished by having a different hidden class type.
    enum HCType {
        NORMAL,      // attributes stored in attributes array, name->offset map stored in hidden class
        DICT_BACKED, // first attribute in array is a dict-like object which stores the attributes
        SINGLETON,   // name->offset map stored in hidden class, but hcls is mutable
    } const type;

    static HiddenClass* dict_backed;

private:
    HiddenClass(HCType type) : type(type) {}
    HiddenClass(HiddenClass* parent)
        : type(NORMAL), attr_offsets(parent->attr_offsets), attrwrapper_offset(parent->attrwrapper_offset) {
        assert(parent->type == NORMAL);
    }

    // These fields only make sense for NORMAL or SINGLETON hidden classes:
    llvm::DenseMap<BoxedString*, int> attr_offsets;
    // If >= 0, is the offset where we stored an attrwrapper object
    int attrwrapper_offset = -1;

    // These are only for NORMAL hidden classes:
    ContiguousMap<BoxedString*, HiddenClass*, llvm::DenseMap<BoxedString*, int>> children;
    HiddenClass* attrwrapper_child = NULL;

    // Only for SINGLETON hidden classes:
    ICInvalidator dependent_getattrs;

public:
    static HiddenClass* makeSingleton() { return new HiddenClass(SINGLETON); }

    static HiddenClass* makeRoot() {
#ifndef NDEBUG
        static bool made = false;
        assert(!made);
        made = true;
#endif
        return new HiddenClass(NORMAL);
    }
    static HiddenClass* makeDictBacked() {
#ifndef NDEBUG
        static bool made = false;
        assert(!made);
        made = true;
#endif
        return new HiddenClass(DICT_BACKED);
    }

    void gc_visit(GCVisitor* visitor) {
        // Visit children even for the dict-backed case, since children will just be empty
        visitor->visitRange((void* const*)&children.vector()[0], (void* const*)&children.vector()[children.size()]);
        visitor->visit(attrwrapper_child);

        // We don't need to visit the keys of the 'children' map, since the children should have those as entries
        // in the attr_offssets map.
        // Also, if we have any children, we can skip scanning our attr_offsets map, since it will be a subset
        // of our child's map.
        if (children.empty())
            for (auto p : attr_offsets)
                visitor->visit(p.first);
    }

    // The total size of the attribute array.  The slots in the attribute array may not correspond 1:1 to Python
    // attributes.
    int attributeArraySize() {
        if (type == DICT_BACKED)
            return 1;

        ASSERT(type == NORMAL || type == SINGLETON, "%d", type);
        int r = attr_offsets.size();
        if (attrwrapper_offset != -1)
            r += 1;
        return r;
    }

    // The mapping from string attribute names to attribute offsets.  There may be other objects in the attributes
    // array.
    // Only valid for NORMAL or SINGLETON hidden classes
    const llvm::DenseMap<BoxedString*, int>& getStrAttrOffsets() {
        assert(type == NORMAL || type == SINGLETON);
        return attr_offsets;
    }

    // Only valid for NORMAL hidden classes:
    HiddenClass* getOrMakeChild(BoxedString* attr);

    // Only valid for NORMAL or SINGLETON hidden classes:
    int getOffset(BoxedString* attr) {
        assert(type == NORMAL || type == SINGLETON);
        auto it = attr_offsets.find(attr);
        if (it == attr_offsets.end())
            return -1;
        return it->second;
    }

    int getAttrwrapperOffset() {
        assert(type == NORMAL || type == SINGLETON);
        return attrwrapper_offset;
    }

    // Only valid for SINGLETON hidden classes:
    void appendAttribute(BoxedString* attr);
    void appendAttrwrapper();
    void delAttribute(BoxedString* attr);
    void addDependence(Rewriter* rewriter);

    // Only valid for NORMAL hidden classes:
    HiddenClass* getAttrwrapperChild();

    // Only valid for NORMAL hidden classes:
    HiddenClass* delAttrToMakeHC(BoxedString* attr);
};
}
#endif
