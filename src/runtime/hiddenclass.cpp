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
// limitations under the License.

#include "runtime/hiddenclass.h"

#include <cassert>

#include "asm_writing/icinfo.h"
#include "asm_writing/rewriter.h"
#include "core/stats.h"
#include "core/types.h"
#include "runtime/objmodel.h"
#include "runtime/types.h"
#include "runtime/util.h"

namespace pyston {

void HiddenClass::appendAttribute(BoxedString* attr) {
    assert(attr->interned_state != SSTATE_NOT_INTERNED);
    assert(type == SINGLETON);
    dependent_getattrs.invalidateAll();
    assert(attr_offsets.count(attr) == 0);
    int n = this->attributeArraySize();
    attr_offsets[attr] = n;
}

void HiddenClass::appendAttrwrapper() {
    assert(type == SINGLETON);
    dependent_getattrs.invalidateAll();
    assert(attrwrapper_offset == -1);
    attrwrapper_offset = this->attributeArraySize();
}

void HiddenClass::delAttribute(BoxedString* attr) {
    assert(attr->interned_state != SSTATE_NOT_INTERNED);
    assert(type == SINGLETON);
    dependent_getattrs.invalidateAll();
    assert(attr_offsets.count(attr));

    int prev_idx = attr_offsets[attr];
    attr_offsets.erase(attr);

    for (auto it = attr_offsets.begin(), end = attr_offsets.end(); it != end; ++it) {
        assert(it->second != prev_idx);
        if (it->second > prev_idx)
            it->second--;
    }
    if (attrwrapper_offset != -1 && attrwrapper_offset > prev_idx)
        attrwrapper_offset--;
}

void HiddenClass::addDependence(Rewriter* rewriter) {
    assert(type == SINGLETON);
    rewriter->addDependenceOn(dependent_getattrs);
}

HiddenClass* HiddenClass::getOrMakeChild(BoxedString* attr) {
    STAT_TIMER(t0, "us_timer_hiddenclass_getOrMakeChild", 0);

    assert(attr->interned_state != SSTATE_NOT_INTERNED);
    assert(type == NORMAL);

    auto it = children.find(attr);
    if (it != children.end())
        return children.getMapped(it->second);

    static StatCounter num_hclses("num_hidden_classes");
    num_hclses.log();

    HiddenClass* rtn = new HiddenClass(this);
    rtn->attr_offsets[attr] = this->attributeArraySize();
    this->children[attr] = rtn;
    assert(rtn->attributeArraySize() == this->attributeArraySize() + 1);
    return rtn;
}

HiddenClass* HiddenClass::getAttrwrapperChild() {
    assert(type == NORMAL);
    assert(attrwrapper_offset == -1);

    if (!attrwrapper_child) {
        HiddenClass* made = new HiddenClass(this);
        made->attrwrapper_offset = this->attributeArraySize();
        this->attrwrapper_child = made;
        assert(made->attributeArraySize() == this->attributeArraySize() + 1);
    }

    return attrwrapper_child;
}

/**
 * del attr from current HiddenClass, maintaining the order of the remaining attrs
 */
HiddenClass* HiddenClass::delAttrToMakeHC(BoxedString* attr) {
    STAT_TIMER(t0, "us_timer_hiddenclass_delAttrToMakeHC", 0);

    assert(attr->interned_state != SSTATE_NOT_INTERNED);
    assert(type == NORMAL);
    int idx = getOffset(attr);
    assert(idx >= 0);

    std::vector<BoxedString*> new_attrs(attributeArraySize() - 1);
    for (auto it = attr_offsets.begin(); it != attr_offsets.end(); ++it) {
        if (it->second < idx)
            new_attrs[it->second] = it->first;
        else if (it->second > idx) {
            new_attrs[it->second - 1] = it->first;
        }
    }

    int new_attrwrapper_offset = attrwrapper_offset;
    if (new_attrwrapper_offset > idx)
        new_attrwrapper_offset--;

    // TODO we can first locate the parent HiddenClass of the deleted
    // attribute and hence avoid creation of its ancestors.
    HiddenClass* cur = root_hcls;
    int curidx = 0;
    for (const auto& attr : new_attrs) {
        if (curidx == new_attrwrapper_offset)
            cur = cur->getAttrwrapperChild();
        else
            cur = cur->getOrMakeChild(attr);
        curidx++;
    }
    return cur;
}

} // namespace pyston
