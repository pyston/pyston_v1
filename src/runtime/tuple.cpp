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

#include <sstream>

#include "core/ast.h"
#include "core/common.h"
#include "core/stats.h"
#include "core/types.h"

#include "runtime/gc_runtime.h"
#include "runtime/objmodel.h"
#include "runtime/types.h"
#include "runtime/util.h"

namespace pyston {

extern "C" Box* createTuple(int64_t nelts, Box* *elts) {
    std::vector<Box*> velts(elts, elts + nelts);
    return new BoxedTuple(velts);
}

void tuple_dtor(BoxedTuple* t) {
    typedef std::vector<Box*> T;
    (&t->elts)->~T();
}

Box* tupleGetitem(BoxedTuple *self, Box* slice) {
    assert(self->cls == tuple_cls);

    i64 size = self->elts.size();

    if (slice->cls == int_cls) {
        i64 n = static_cast<BoxedInt*>(slice)->n;

        if (n < 0) n = size - n;
        if (n < 0 || n >= size) {
            fprintf(stderr, "indexerror\n");
            raiseExc();
        }

        Box* rtn = self->elts[n];
        return rtn;
    } else {
        RELEASE_ASSERT(0, "");
    }
}

Box* tupleLen(BoxedTuple *t) {
    assert(t->cls == tuple_cls);
    return boxInt(t->elts.size());
}

Box* tupleRepr(BoxedTuple *t) {
    assert(t->cls == tuple_cls);

    std::ostringstream os("");
    os << "(";

    int n = t->elts.size();
    for (int i = 0; i < n; i++) {
        if (i) os << ", ";

        BoxedString *elt_repr = static_cast<BoxedString*>(repr(t->elts[i]));
        os << elt_repr->s;
    }
    if (n == 1) os << ",";
    os << ")";

    return boxString(os.str());
}

Box* _tupleCmp(BoxedTuple *lhs, BoxedTuple *rhs, AST_TYPE::AST_TYPE op_type) {
    int lsz = lhs->elts.size();
    int rsz = rhs->elts.size();

    bool is_order = (op_type == AST_TYPE::Lt || op_type == AST_TYPE::LtE || op_type == AST_TYPE::Gt || op_type == AST_TYPE::GtE);

    int n = std::min(lsz, rsz);
    for (int i = 0; i < n; i++) {
        Box* is_eq = compareInternal(lhs->elts[i], rhs->elts[i], AST_TYPE::Eq, NULL);
        bool bis_eq = nonzero(is_eq);

        if (bis_eq) continue;

        if (op_type == AST_TYPE::Eq) {
            return boxBool(false);
        } else if (op_type == AST_TYPE::NotEq) {
            return boxBool(true);
        } else {
            Box* r = compareInternal(lhs->elts[i], rhs->elts[i], op_type, NULL);
            return r;
        }
    }

    if (op_type == AST_TYPE::Lt)
        return boxBool(lsz < rsz);
    else if (op_type == AST_TYPE::LtE)
        return boxBool(lsz <= rsz);
    else if (op_type == AST_TYPE::Gt)
        return boxBool(lsz > rsz);
    else if (op_type == AST_TYPE::GtE)
        return boxBool(lsz >= rsz);
    else if (op_type == AST_TYPE::Eq)
        return boxBool(lsz == rsz);
    else if (op_type == AST_TYPE::NotEq)
        return boxBool(lsz != rsz);

    RELEASE_ASSERT(0, "%d", op_type);
}

Box* tupleLt(BoxedTuple *self, Box *rhs) {
    if (rhs->cls != tuple_cls) {
        return NotImplemented;
    }
    return _tupleCmp(self, static_cast<BoxedTuple*>(rhs), AST_TYPE::Lt);
}

Box* tupleLe(BoxedTuple *self, Box *rhs) {
    if (rhs->cls != tuple_cls) {
        return NotImplemented;
    }
    return _tupleCmp(self, static_cast<BoxedTuple*>(rhs), AST_TYPE::LtE);
}

Box* tupleGt(BoxedTuple *self, Box *rhs) {
    if (rhs->cls != tuple_cls) {
        return NotImplemented;
    }
    return _tupleCmp(self, static_cast<BoxedTuple*>(rhs), AST_TYPE::Gt);
}

Box* tupleGe(BoxedTuple *self, Box *rhs) {
    if (rhs->cls != tuple_cls) {
        return NotImplemented;
    }
    return _tupleCmp(self, static_cast<BoxedTuple*>(rhs), AST_TYPE::GtE);
}

Box* tupleEq(BoxedTuple *self, Box *rhs) {
    if (rhs->cls != tuple_cls) {
        return NotImplemented;
    }
    return _tupleCmp(self, static_cast<BoxedTuple*>(rhs), AST_TYPE::Eq);
}

Box* tupleNe(BoxedTuple *self, Box *rhs) {
    if (rhs->cls != tuple_cls) {
        return NotImplemented;
    }
    return _tupleCmp(self, static_cast<BoxedTuple*>(rhs), AST_TYPE::NotEq);
}

Box* tupleContains(BoxedTuple* self, Box *elt) {
    int size = self->elts.size();
    for (int i = 0; i < size; i++) {
        Box* e = self->elts[i];
        Box* cmp = compareInternal(e, elt, AST_TYPE::Eq, NULL);
        bool b = nonzero(cmp);
        if (b)
            return True;
    }
    return False;
}

void setupTuple() {
    tuple_cls->giveAttr("__name__", boxStrConstant("tuple"));

    tuple_cls->giveAttr("__getitem__", new BoxedFunction(boxRTFunction((void*)tupleGetitem, NULL, 2, false)));
    tuple_cls->giveAttr("__contains__", new BoxedFunction(boxRTFunction((void*)tupleContains, NULL, 2, false)));

    tuple_cls->giveAttr("__lt__", new BoxedFunction(boxRTFunction((void*)tupleLt, NULL, 2, false)));
    tuple_cls->giveAttr("__le__", new BoxedFunction(boxRTFunction((void*)tupleLe, NULL, 2, false)));
    tuple_cls->giveAttr("__gt__", new BoxedFunction(boxRTFunction((void*)tupleGt, NULL, 2, false)));
    tuple_cls->giveAttr("__ge__", new BoxedFunction(boxRTFunction((void*)tupleGe, NULL, 2, false)));
    tuple_cls->giveAttr("__eq__", new BoxedFunction(boxRTFunction((void*)tupleEq, NULL, 2, false)));
    tuple_cls->giveAttr("__ne__", new BoxedFunction(boxRTFunction((void*)tupleNe, NULL, 2, false)));

    tuple_cls->giveAttr("__len__", new BoxedFunction(boxRTFunction((void*)tupleLen, NULL, 1, false)));
    tuple_cls->giveAttr("__repr__", new BoxedFunction(boxRTFunction((void*)tupleRepr, NULL, 1, false)));
    tuple_cls->setattr("__str__", tuple_cls->peekattr("__repr__"), NULL, NULL);

    tuple_cls->freeze();
}

void teardownTuple() {
}

}
