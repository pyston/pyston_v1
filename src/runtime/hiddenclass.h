// Copyright (c) 2014-2016 Dropbox, Inc.
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

#include "core/from_llvm/DenseMap.h"
#include "core/types.h"

namespace pyston {

class HiddenClassDict;
class HiddenClassNormal;
class HiddenClassSingleton;
class HiddenClassSingletonOrNormal;
class HiddenClass {
public:
    // We have a couple different storage strategies for attributes, which
    // are distinguished by having a different hidden class type.
    enum HCType : unsigned char {
        NORMAL,      // attributes stored in attributes array, name->offset map stored in hidden class
        DICT_BACKED, // first attribute in array is a dict-like object which stores the attributes
        SINGLETON,   // name->offset map stored in hidden class, but hcls is mutable
    } const type;

    static HiddenClass* dict_backed;
    void dump() noexcept;

protected:
    HiddenClass(HCType type) : type(type) {}
    virtual ~HiddenClass() = default;

public:
    static HiddenClassSingleton* makeSingleton();
    static HiddenClassNormal* makeRoot();
    static HiddenClassDict* makeDictBacked();

    int attributeArraySize();

    HiddenClassDict* getAsDictBacked() {
        assert(type == HiddenClass::DICT_BACKED);
        return reinterpret_cast<HiddenClassDict*>(this);
    }

    HiddenClassNormal* getAsNormal() {
        assert(type == HiddenClass::NORMAL);
        return reinterpret_cast<HiddenClassNormal*>(this);
    }

    HiddenClassSingleton* getAsSingleton() {
        assert(type == HiddenClass::SINGLETON);
        return reinterpret_cast<HiddenClassSingleton*>(this);
    }

    HiddenClassSingletonOrNormal* getAsSingletonOrNormal() {
        assert(type == HiddenClass::NORMAL || type == HiddenClass::SINGLETON);
        return reinterpret_cast<HiddenClassSingletonOrNormal*>(this);
    }
};

class HiddenClassDict final : public HiddenClass {
protected:
    HiddenClassDict() : HiddenClass(HiddenClass::DICT_BACKED) {}

public:
    // The total size of the attribute array.  The slots in the attribute array may not correspond 1:1 to Python
    // attributes.
    int attributeArraySize() { return 1; }

    friend class HiddenClass;
};

class HiddenClassSingletonOrNormal : public HiddenClass {
protected:
    HiddenClassSingletonOrNormal(HCType type) : HiddenClass(type) {}

    HiddenClassSingletonOrNormal(HiddenClassSingletonOrNormal* parent)
        : HiddenClass(HiddenClass::NORMAL),
          attrwrapper_offset(parent->attrwrapper_offset),
          attr_offsets(parent->attr_offsets) {
        assert(parent->type == HiddenClass::NORMAL);
    }

    // If >= 0, is the offset where we stored an attrwrapper object
    int attrwrapper_offset = -1;

    typedef pyston::DenseMap<BoxedString*, int, pyston::DenseMapInfo<BoxedString*>,
                             pyston::detail::DenseMapPair<BoxedString*, int>, 16> Map;
    Map attr_offsets;

public:
    // The mapping from string attribute names to attribute offsets.  There may be other objects in the attributes
    // array.
    BORROWED(const Map&) getStrAttrOffsets() {
        assert(type == NORMAL || type == SINGLETON);
        return attr_offsets;
    }

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

    // The total size of the attribute array.  The slots in the attribute array may not correspond 1:1 to Python
    // attributes.
    int attributeArraySize() {
        ASSERT(type == NORMAL || type == SINGLETON, "%d", type);
        int r = attr_offsets.size();
        if (attrwrapper_offset != -1)
            r += 1;
        return r;
    }

    friend class HiddenClass;
};

class HiddenClassNormal final : public HiddenClassSingletonOrNormal {
protected:
    HiddenClassNormal() : HiddenClassSingletonOrNormal(HiddenClass::NORMAL) {}
    HiddenClassNormal(HiddenClassNormal* parent) : HiddenClassSingletonOrNormal(parent) {}

    pyston::SmallDenseMap<BoxedString*, HiddenClassNormal*> children;
    HiddenClassNormal* attrwrapper_child = NULL;

public:
    HiddenClassNormal* getOrMakeChild(BoxedString* attr);
    HiddenClassNormal* getAttrwrapperChild();
    HiddenClassNormal* delAttrToMakeHC(BoxedString* attr);

    friend class HiddenClass;
};

class HiddenClassSingleton final : public HiddenClassSingletonOrNormal {
protected:
    HiddenClassSingleton() : HiddenClassSingletonOrNormal(HiddenClass::SINGLETON) {}

    ICInvalidator dependent_getattrs;

public:
    void appendAttribute(BoxedString* attr);
    void appendAttrwrapper();
    void delAttribute(BoxedString* attr);
    void addDependence(Rewriter* rewriter);

    friend class HiddenClass;
};
}
#endif
