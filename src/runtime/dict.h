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

#ifndef PYSTON_RUNTIME_DICT_H
#define PYSTON_RUNTIME_DICT_H

#include "core/types.h"
#include "runtime/types.h"

namespace pyston {

extern BoxedClass* dict_iterator_cls;
extern BoxedClass* dict_keys_cls;
extern BoxedClass* dict_values_cls;
extern BoxedClass* dict_items_cls;

class BoxedDictIterator : public Box {
public:
    enum IteratorType { KeyIterator, ValueIterator, ItemIterator };

    BoxedDict* d;
    BoxedDict::DictMap::iterator it;
    const BoxedDict::DictMap::iterator itEnd;
    const IteratorType type;

    BoxedDictIterator(BoxedDict* d, IteratorType type);

    DEFAULT_CLASS(dict_iterator_cls);

    static void gcHandler(GCVisitor* v, Box* self);
};

Box* dictGetitem(BoxedDict* self, Box* k);

Box* dict_iter(Box* s) noexcept;
Box* dictIterKeys(Box* self);
Box* dictIterValues(Box* self);
Box* dictIterItems(Box* self);
Box* dictIterIter(Box* self);
Box* dictIterHasnext(Box* self);
i1 dictIterHasnextUnboxed(Box* self);
Box* dictiter_next(Box* self) noexcept;
Box* dictIterNext(Box* self);

class BoxedDictView : public Box {
public:
    BoxedDict* d;
    BoxedDictView(BoxedDict* d);

    static void gcHandler(GCVisitor* v, Box* self);
};

Box* dictViewKeysIter(Box* self);
Box* dictViewValuesIter(Box* self);
Box* dictViewItemsIter(Box* self);
void dictMerge(BoxedDict* self, Box* other);
Box* dictUpdate(BoxedDict* self, BoxedTuple* args, BoxedDict* kwargs);
}

#endif
