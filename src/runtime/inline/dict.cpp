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

#include <cstring>

#include "runtime/dict.h"
#include "runtime/gc_runtime.h"

namespace pyston {

BoxedDictIterator::BoxedDictIterator(BoxedDict* d, IteratorType type)
    : Box(&dict_iterator_flavor, dict_iterator_cls), d(d), it(d->d.begin()), itEnd(d->d.end()), type(type) {
}

Box* dictIterKeys(Box* s) {
    assert(s->cls == dict_cls);
    BoxedDict* self = static_cast<BoxedDict*>(s);
    return new BoxedDictIterator(self, BoxedDictIterator::KeyIterator);
}

Box* dictIterValues(Box* s) {
    assert(s->cls == dict_cls);
    BoxedDict* self = static_cast<BoxedDict*>(s);
    return new BoxedDictIterator(self, BoxedDictIterator::ValueIterator);
}

Box* dictIterItems(Box* s) {
    assert(s->cls == dict_cls);
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

Box* dictIterNext(Box* s) {
    assert(s->cls == dict_iterator_cls);
    BoxedDictIterator* self = static_cast<BoxedDictIterator*>(s);

    Box* rtn = nullptr;
    if (self->type == BoxedDictIterator::KeyIterator) {
        rtn = self->it->first;
    } else if (self->type == BoxedDictIterator::ValueIterator) {
        rtn = self->it->second;
    } else if (self->type == BoxedDictIterator::ItemIterator) {
        BoxedTuple::GCVector elts{ self->it->first, self->it->second };
        rtn = new BoxedTuple(std::move(elts));
    }
    ++self->it;
    return rtn;
}
}
