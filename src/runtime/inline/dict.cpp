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

#include <cstring>

#include "runtime/dict.h"
#include "runtime/objmodel.h"

namespace pyston {

BoxedDictIterator::BoxedDictIterator(BoxedDict* d, IteratorType type)
    : d(d), it(d->d.begin()), itEnd(d->d.end()), type(type) {
}

Box* dict_iter(Box* s) noexcept {
    assert(PyDict_Check(s));
    BoxedDict* self = static_cast<BoxedDict*>(s);
    return new BoxedDictIterator(self, BoxedDictIterator::KeyIterator);
}

Box* dictIterKeys(Box* s) {
    return dict_iter(s);
}

Box* dictIterValues(Box* s) {
    assert(PyDict_Check(s));
    BoxedDict* self = static_cast<BoxedDict*>(s);
    return new BoxedDictIterator(self, BoxedDictIterator::ValueIterator);
}

Box* dictIterItems(Box* s) {
    assert(PyDict_Check(s));
    BoxedDict* self = static_cast<BoxedDict*>(s);
    return new BoxedDictIterator(self, BoxedDictIterator::ItemIterator);
}

Box* dictIterIter(Box* s) {
    return s;
}

i1 dictIterHasnextUnboxed(Box* s) {
    assert(s->cls == dict_iterator_cls);
    BoxedDictIterator* self = static_cast<BoxedDictIterator*>(s);

    return self->it != self->itEnd;
}

Box* dictIterHasnext(Box* s) {
    return boxBool(dictIterHasnextUnboxed(s));
}

Box* dictiter_next(Box* s) noexcept {
    assert(s->cls == dict_iterator_cls);
    BoxedDictIterator* self = static_cast<BoxedDictIterator*>(s);

    if (self->it == self->itEnd)
        return NULL;

    Box* rtn = nullptr;
    if (self->type == BoxedDictIterator::KeyIterator) {
        rtn = self->it->first.value;
    } else if (self->type == BoxedDictIterator::ValueIterator) {
        rtn = self->it->second;
    } else if (self->type == BoxedDictIterator::ItemIterator) {
        rtn = BoxedTuple::create({ self->it->first.value, self->it->second });
    }
    ++self->it;
    return rtn;
}

Box* dictIterNext(Box* s) {
    auto* rtn = dictiter_next(s);
    if (!rtn)
        raiseExcHelper(StopIteration, "");
    return rtn;
}

BoxedDictView::BoxedDictView(BoxedDict* d) : d(d) {
}

Box* dictViewKeysIter(Box* s) {
    assert(s->cls == dict_keys_cls);
    BoxedDictView* self = static_cast<BoxedDictView*>(s);
    return dictIterKeys(self->d);
}

Box* dictViewValuesIter(Box* s) {
    assert(s->cls == dict_values_cls);
    BoxedDictView* self = static_cast<BoxedDictView*>(s);
    return dictIterValues(self->d);
}

Box* dictViewItemsIter(Box* s) {
    assert(s->cls == dict_items_cls);
    BoxedDictView* self = static_cast<BoxedDictView*>(s);
    return dictIterItems(self->d);
}
}
