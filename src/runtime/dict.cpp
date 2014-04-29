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

#include "core/common.h"
#include "core/stats.h"
#include "core/types.h"

#include "runtime/gc_runtime.h"
#include "runtime/objmodel.h"
#include "runtime/types.h"
#include "runtime/util.h"

namespace pyston {

Box* dictRepr(BoxedDict* self) {
    std::vector<char> chars;
    chars.push_back('{');
    bool first = true;
    for (const auto &p : self->d) {
        if (!first) {
            chars.push_back(',');
            chars.push_back(' ');
        }
        first = false;

        BoxedString *k = repr(p.first);
        BoxedString *v = repr(p.second);
        chars.insert(chars.end(), k->s.begin(), k->s.end());
        chars.push_back(':');
        chars.push_back(' ');
        chars.insert(chars.end(), v->s.begin(), v->s.end());
    }
    chars.push_back('}');
    return boxString(std::string(chars.begin(), chars.end()));
}

Box* dictItems(BoxedDict* self) {
    BoxedList* rtn = new BoxedList();

    for (const auto &p : self->d) {
        std::vector<Box*> elts;
        elts.push_back(p.first);
        elts.push_back(p.second);
        BoxedTuple *t = new BoxedTuple(elts);
        listAppendInternal(rtn, t);
    }

    return rtn;
}

Box* dictValues(BoxedDict* self) {
    BoxedList* rtn = new BoxedList();
    for (const auto &p : self->d) {
        listAppendInternal(rtn, p.second);
    }
    return rtn;
}

Box* dictKeys(BoxedDict* self) {
    BoxedList* rtn = new BoxedList();
    for (const auto &p : self->d) {
        listAppendInternal(rtn, p.first);
    }
    return rtn;
}

Box* dictGetitem(BoxedDict* self, Box* k) {
    Box* &pos = self->d[k];

    if (pos == NULL) {
        BoxedString *s = repr(k);
        fprintf(stderr, "KeyError: %s\n", s->s.c_str());
        raiseExc();
    }

    return pos;
}

Box* dictSetitem(BoxedDict* self, Box* k, Box* v) {
    //printf("Starting setitem\n");
    Box* &pos = self->d[k];
    //printf("Got the pos\n");

    if (pos != NULL) {
        pos = v;
    } else {
        pos = v;
    }

    return None;
}

void dict_dtor(BoxedDict* self) {
    self->d.clear();

    // I thought, in disbelief, that this works:
    //(&self->d)->~decltype(self->d)();
    // but that's only on clang, so instead do this:
    typedef decltype(self->d) T;
    (&self->d)->~T();
}

void setupDict() {
    dict_cls->giveAttr("__name__", boxStrConstant("dict"));
    //dict_cls->giveAttr("__len__", new BoxedFunction(boxRTFunction((void*)dictLen, NULL, 1, false)));
    //dict_cls->giveAttr("__getitem__", new BoxedFunction(boxRTFunction((void*)dictGetitem, NULL, 2, false)));
    //dict_cls->giveAttr("__new__", new BoxedFunction(boxRTFunction((void*)dictNew, NULL, 1, false)));
    //dict_cls->giveAttr("__init__", new BoxedFunction(boxRTFunction((void*)dictInit, NULL, 1, false)));
    dict_cls->giveAttr("__repr__", new BoxedFunction(boxRTFunction((void*)dictRepr, NULL, 1, false)));
    dict_cls->setattr("__str__", dict_cls->peekattr("__repr__"), NULL, NULL);

    dict_cls->giveAttr("items", new BoxedFunction(boxRTFunction((void*)dictItems, NULL, 1, false)));
    dict_cls->setattr("iteritems", dict_cls->peekattr("items"), NULL, NULL);

    dict_cls->giveAttr("values", new BoxedFunction(boxRTFunction((void*)dictValues, NULL, 1, false)));
    dict_cls->setattr("itervalues", dict_cls->peekattr("values"), NULL, NULL);

    dict_cls->giveAttr("keys", new BoxedFunction(boxRTFunction((void*)dictKeys, NULL, 1, false)));
    dict_cls->setattr("iterkeys", dict_cls->peekattr("keys"), NULL, NULL);

    dict_cls->giveAttr("__getitem__", new BoxedFunction(boxRTFunction((void*)dictGetitem, NULL, 2, false)));
    dict_cls->giveAttr("__setitem__", new BoxedFunction(boxRTFunction((void*)dictSetitem, NULL, 3, false)));

    dict_cls->freeze();
}

void teardownDict() {
}

}

