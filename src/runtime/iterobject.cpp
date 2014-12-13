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

#include "runtime/iterobject.h"

#include <cmath>
#include <sstream>

#include "core/common.h"
#include "core/options.h"
#include "core/stats.h"
#include "core/types.h"
#include "gc/collector.h"
#include "runtime/inline/boxing.h"
#include "runtime/objmodel.h"
#include "runtime/types.h"
#include "runtime/util.h"

namespace pyston {

BoxedClass* seqiter_cls;

Box* seqiterHasnext(Box* s) {
    RELEASE_ASSERT(s->cls == seqiter_cls, "");
    BoxedSeqIter* self = static_cast<BoxedSeqIter*>(s);

    Box* next;
    try {
        next = getitem(self->b, boxInt(self->idx));
    } catch (Box* b) {
        return False;
    }
    self->idx++;
    self->next = next;
    return True;
}

Box* seqiterNext(Box* s) {
    RELEASE_ASSERT(s->cls == seqiter_cls, "");
    BoxedSeqIter* self = static_cast<BoxedSeqIter*>(s);

    RELEASE_ASSERT(self->next, "");
    Box* r = self->next;
    self->next = NULL;
    return r;
}

void setupIter() {
    seqiter_cls = new BoxedHeapClass(type_cls, object_cls, NULL, 0, sizeof(BoxedSeqIter), false);
    seqiter_cls->giveAttr("__name__", boxStrConstant("iterator"));

    seqiter_cls->giveAttr("next", new BoxedFunction(boxRTFunction((void*)seqiterNext, UNKNOWN, 1)));
    seqiter_cls->giveAttr("__hasnext__", new BoxedFunction(boxRTFunction((void*)seqiterHasnext, BOXED_BOOL, 1)));

    seqiter_cls->freeze();
}
}
