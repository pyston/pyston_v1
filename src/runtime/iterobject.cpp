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
BoxedClass* iterwrapper_cls;

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

static void iterwrapperGCVisit(GCVisitor* v, Box* b) {
    assert(b->cls == iterwrapper_cls);
    boxGCHandler(v, b);

    BoxedIterWrapper* iw = static_cast<BoxedIterWrapper*>(b);
    if (iw->next)
        v->visit(iw->next);
}

Box* iterwrapperHasnext(Box* s) {
    RELEASE_ASSERT(s->cls == iterwrapper_cls, "");
    BoxedIterWrapper* self = static_cast<BoxedIterWrapper*>(s);

    static const std::string next_str("next");
    Box* next;
    try {
        next = callattr(self->iter, &next_str, CallattrFlags({.cls_only = true, .null_on_nonexistent = false }),
                        ArgPassSpec(0), NULL, NULL, NULL, NULL, NULL);
    } catch (Box* b) {
        if (isSubclass(b->cls, StopIteration)) {
            self->next = NULL;
            return False;
        }
        throw;
    }
    self->next = next;
    return True;
}

Box* iterwrapperNext(Box* s) {
    RELEASE_ASSERT(s->cls == iterwrapper_cls, "");
    BoxedIterWrapper* self = static_cast<BoxedIterWrapper*>(s);

    RELEASE_ASSERT(self->next, "");
    Box* r = self->next;
    self->next = NULL;
    return r;
}

void setupIter() {
    seqiter_cls = new BoxedHeapClass(object_cls, NULL, 0, sizeof(BoxedSeqIter), false);
    seqiter_cls->giveAttr("__name__", boxStrConstant("iterator"));

    seqiter_cls->giveAttr("next", new BoxedFunction(boxRTFunction((void*)seqiterNext, UNKNOWN, 1)));
    seqiter_cls->giveAttr("__hasnext__", new BoxedFunction(boxRTFunction((void*)seqiterHasnext, BOXED_BOOL, 1)));

    seqiter_cls->freeze();

    iterwrapper_cls = new BoxedHeapClass(object_cls, iterwrapperGCVisit, 0, sizeof(BoxedIterWrapper), false);
    iterwrapper_cls->giveAttr("__name__", boxStrConstant("iterwrapper"));

    iterwrapper_cls->giveAttr("next", new BoxedFunction(boxRTFunction((void*)iterwrapperNext, UNKNOWN, 1)));
    iterwrapper_cls->giveAttr("__hasnext__",
                              new BoxedFunction(boxRTFunction((void*)iterwrapperHasnext, BOXED_BOOL, 1)));

    iterwrapper_cls->freeze();
}
}
