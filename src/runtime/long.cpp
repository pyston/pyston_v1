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

#include "runtime/long.h"

#include <cmath>
#include <gmp.h>
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

BoxedClass* long_cls;

static int64_t asSignedLong(BoxedLong* self) {
    assert(self->cls == long_cls);
    if (!mpz_fits_slong_p(self->n))
        raiseExcHelper(OverflowError, "long int too large to convert to int");
    return mpz_get_si(self->n);
}

static uint64_t asUnsignedLong(BoxedLong* self) {
    assert(self->cls == long_cls);

    // if this is ever true, we should raise a Python error, but I don't think we should hit it?
    assert(mpz_cmp_si(self->n, 0) >= 0);

    if (!mpz_fits_ulong_p(self->n))
        raiseExcHelper(OverflowError, "long int too large to convert to int");
    return mpz_get_ui(self->n);
}

extern "C" unsigned long PyLong_AsUnsignedLong(PyObject* vv) {
    RELEASE_ASSERT(PyLong_Check(vv), "");
    BoxedLong* l = static_cast<BoxedLong*>(vv);

    try {
        return asUnsignedLong(l);
    } catch (Box* e) {
        abort();
    }
}

extern "C" long PyLong_AsLong(PyObject* vv) {
    RELEASE_ASSERT(PyLong_Check(vv), "");
    BoxedLong* l = static_cast<BoxedLong*>(vv);
    RELEASE_ASSERT(mpz_fits_slong_p(l->n), "");
    return mpz_get_si(l->n);
}

extern "C" long PyLong_AsLongAndOverflow(PyObject*, int*) {
    Py_FatalError("unimplemented");
}

extern "C" double PyLong_AsDouble(PyObject* vv) {
    RELEASE_ASSERT(PyLong_Check(vv), "");
    BoxedLong* l = static_cast<BoxedLong*>(vv);
    return mpz_get_d(l->n);
}

extern "C" PyObject* PyLong_FromDouble(double v) {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyLong_FromLong(long ival) {
    BoxedLong* rtn = new BoxedLong(long_cls);
    mpz_init_set_si(rtn->n, ival);
    return rtn;
}

extern "C" PyObject* PyLong_FromUnsignedLong(unsigned long ival) {
    BoxedLong* rtn = new BoxedLong(long_cls);
    mpz_init_set_ui(rtn->n, ival);
    return rtn;
}

extern "C" double _PyLong_Frexp(PyLongObject* a, Py_ssize_t* e) {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* _PyLong_FromByteArray(const unsigned char* bytes, size_t n, int little_endian, int is_signed) {
    if (n == 0)
        return PyLong_FromLong(0);

    if (is_signed) {
        Py_FatalError("unimplemented");
        return 0;
    }

    if (!little_endian) {
        // TODO: check if the behaviour of mpz_import is right when big endian is specified.
        Py_FatalError("unimplemented");
        return 0;
    }

    BoxedLong* rtn = new BoxedLong(long_cls);
    mpz_init(rtn->n);
    mpz_import(rtn->n, 1, 1, n, little_endian ? -1 : 1, 0, &bytes[0]);
    return rtn;
}

extern "C" Box* createLong(const std::string* s) {
    BoxedLong* rtn = new BoxedLong(long_cls);
    int r = mpz_init_set_str(rtn->n, s->c_str(), 10);
    RELEASE_ASSERT(r == 0, "%d: '%s'", r, s->c_str());
    return rtn;
}

extern "C" BoxedLong* boxLong(int64_t n) {
    BoxedLong* rtn = new BoxedLong(long_cls);
    mpz_init_set_si(rtn->n, n);
    return rtn;
}

extern "C" PyObject* PyLong_FromLongLong(long long ival) {
    BoxedLong* rtn = new BoxedLong(long_cls);
    mpz_init_set_si(rtn->n, ival);
    return rtn;
}

extern "C" PyObject* PyLong_FromUnsignedLongLong(unsigned long long ival) {
    BoxedLong* rtn = new BoxedLong(long_cls);
    mpz_init_set_ui(rtn->n, ival);
    return rtn;
}

extern "C" Box* longNew(Box* _cls, Box* val, Box* _base) {
    if (!isSubclass(_cls->cls, type_cls))
        raiseExcHelper(TypeError, "long.__new__(X): X is not a type object (%s)", getTypeName(_cls)->c_str());

    BoxedClass* cls = static_cast<BoxedClass*>(_cls);
    if (!isSubclass(cls, long_cls))
        raiseExcHelper(TypeError, "long.__new__(%s): %s is not a subtype of long", getNameOfClass(cls)->c_str(),
                       getNameOfClass(cls)->c_str());

    assert(cls->tp_basicsize >= sizeof(BoxedInt));
    void* mem = gc_alloc(cls->tp_basicsize, gc::GCKind::PYTHON);
    BoxedLong* rtn = ::new (mem) BoxedLong(cls);
    initUserAttrs(rtn, cls);

    if (_base) {
        if (!isSubclass(_base->cls, int_cls))
            raiseExcHelper(TypeError, "an integer is required");
        int base = static_cast<BoxedInt*>(_base)->n;

        if (!isSubclass(val->cls, str_cls))
            raiseExcHelper(TypeError, "long() can't convert non-string with explicit base");
        BoxedString* s = static_cast<BoxedString*>(val);

        if (base == 0) {
            // mpz_init_set_str has the ability to auto-detect the base, but I doubt it's
            // quite the same as Python's (ex might be missing octal or binary)
            Py_FatalError("unimplemented");
        }

        if (base < 2 || base > 36) {
            raiseExcHelper(TypeError, "long() arg2 must be >= 2 and <= 36");
        }

        int r = mpz_init_set_str(rtn->n, s->s.c_str(), base);
        RELEASE_ASSERT(r == 0, "");
    } else {
        if (val->cls == int_cls) {
            mpz_init_set_si(rtn->n, static_cast<BoxedInt*>(val)->n);
        } else if (val->cls == str_cls) {
            const std::string& s = static_cast<BoxedString*>(val)->s;
            int r = mpz_init_set_str(rtn->n, s.c_str(), 10);
            RELEASE_ASSERT(r == 0, "");
        } else {
            fprintf(stderr, "TypeError: int() argument must be a string or a number, not '%s'\n",
                    getTypeName(val)->c_str());
            raiseExcHelper(TypeError, "");
        }
    }
    return rtn;
}

Box* longRepr(BoxedLong* v) {
    if (!isSubclass(v->cls, long_cls))
        raiseExcHelper(TypeError, "descriptor '__repr__' requires a 'long' object but received a '%s'",
                       getTypeName(v)->c_str());

    int space_required = mpz_sizeinbase(v->n, 10) + 2; // basic size
    space_required += 1;                               // 'L' suffix
    char* buf = (char*)malloc(space_required);
    mpz_get_str(buf, 10, v->n);
    strcat(buf, "L");

    auto rtn = new BoxedString(buf);
    free(buf);

    return rtn;
}

Box* longStr(BoxedLong* v) {
    if (!isSubclass(v->cls, long_cls))
        raiseExcHelper(TypeError, "descriptor '__str__' requires a 'long' object but received a '%s'",
                       getTypeName(v)->c_str());

    char* buf = mpz_get_str(NULL, 10, v->n);
    auto rtn = new BoxedString(buf);
    free(buf);

    return rtn;
}

Box* longNeg(BoxedLong* v1) {
    if (!isSubclass(v1->cls, long_cls))
        raiseExcHelper(TypeError, "descriptor '__neg__' requires a 'long' object but received a '%s'",
                       getTypeName(v1)->c_str());

    BoxedLong* r = new BoxedLong(long_cls);
    mpz_init(r->n);
    mpz_neg(r->n, v1->n);
    return r;
}

Box* longAbs(BoxedLong* v1) {
    assert(isSubclass(v1->cls, long_cls));
    BoxedLong* r = new BoxedLong(long_cls);
    mpz_init(r->n);
    mpz_abs(r->n, v1->n);
    return r;
}

Box* longAdd(BoxedLong* v1, Box* _v2) {
    if (!isSubclass(v1->cls, long_cls))
        raiseExcHelper(TypeError, "descriptor '__add__' requires a 'long' object but received a '%s'",
                       getTypeName(v1)->c_str());

    if (isSubclass(_v2->cls, long_cls)) {
        BoxedLong* v2 = static_cast<BoxedLong*>(_v2);

        BoxedLong* r = new BoxedLong(long_cls);
        mpz_init(r->n);
        mpz_add(r->n, v1->n, v2->n);
        return r;
    } else if (isSubclass(_v2->cls, int_cls)) {
        BoxedInt* v2 = static_cast<BoxedInt*>(_v2);

        BoxedLong* r = new BoxedLong(long_cls);
        mpz_init(r->n);
        if (v2->n >= 0)
            mpz_add_ui(r->n, v1->n, v2->n);
        else
            mpz_sub_ui(r->n, v1->n, -v2->n);
        return r;
    } else {
        return NotImplemented;
    }
}

extern "C" Box* longAnd(BoxedLong* v1, Box* _v2) {
    if (!isSubclass(v1->cls, long_cls))
        raiseExcHelper(TypeError, "descriptor '__and__' requires a 'long' object but received a '%s'",
                       getTypeName(v1)->c_str());
    if (isSubclass(_v2->cls, long_cls)) {
        BoxedLong* v2 = static_cast<BoxedLong*>(_v2);
        BoxedLong* r = new BoxedLong(long_cls);
        mpz_init(r->n);
        mpz_and(r->n, v1->n, v2->n);
        return r;
    } else if (isSubclass(_v2->cls, int_cls)) {
        BoxedInt* v2_int = static_cast<BoxedInt*>(_v2);
        BoxedLong* r = new BoxedLong(long_cls);
        mpz_init(r->n);
        mpz_t v2_long;
        mpz_init(v2_long);
        if (v2_int->n >= 0)
            mpz_init_set_ui(v2_long, v2_int->n);
        else
            mpz_init_set_si(v2_long, v2_int->n);

        mpz_and(r->n, v1->n, v2_long);
        return r;
    }
    return NotImplemented;
}

extern "C" Box* longXor(BoxedLong* v1, Box* _v2) {
    if (!isSubclass(v1->cls, long_cls))
        raiseExcHelper(TypeError, "descriptor '__xor__' requires a 'long' object but received a '%s'",
                       getTypeName(v1)->c_str());
    if (isSubclass(_v2->cls, long_cls)) {
        BoxedLong* v2 = static_cast<BoxedLong*>(_v2);
        BoxedLong* r = new BoxedLong(long_cls);
        mpz_init(r->n);
        mpz_xor(r->n, v1->n, v2->n);
        return r;
    } else if (isSubclass(_v2->cls, int_cls)) {
        BoxedInt* v2_int = static_cast<BoxedInt*>(_v2);
        BoxedLong* r = new BoxedLong(long_cls);
        mpz_init(r->n);
        mpz_t v2_long;
        mpz_init(v2_long);
        if (v2_int->n >= 0)
            mpz_init_set_ui(v2_long, v2_int->n);
        else
            mpz_init_set_si(v2_long, v2_int->n);

        mpz_xor(r->n, v1->n, v2_long);
        return r;
    }
    return NotImplemented;
}

// TODO reduce duplication between these 6 functions, and add double support
Box* longGt(BoxedLong* v1, Box* _v2) {
    if (!isSubclass(v1->cls, long_cls))
        raiseExcHelper(TypeError, "descriptor '__gt__' requires a 'long' object but received a '%s'",
                       getTypeName(v1)->c_str());

    if (isSubclass(_v2->cls, long_cls)) {
        BoxedLong* v2 = static_cast<BoxedLong*>(_v2);

        return boxBool(mpz_cmp(v1->n, v2->n) > 0);
    } else if (isSubclass(_v2->cls, int_cls)) {
        BoxedInt* v2 = static_cast<BoxedInt*>(_v2);

        return boxBool(mpz_cmp_si(v1->n, v2->n) > 0);
    } else {
        return NotImplemented;
    }
}

Box* longGe(BoxedLong* v1, Box* _v2) {
    if (!isSubclass(v1->cls, long_cls))
        raiseExcHelper(TypeError, "descriptor '__ge__' requires a 'long' object but received a '%s'",
                       getTypeName(v1)->c_str());

    if (isSubclass(_v2->cls, long_cls)) {
        BoxedLong* v2 = static_cast<BoxedLong*>(_v2);

        return boxBool(mpz_cmp(v1->n, v2->n) >= 0);
    } else if (isSubclass(_v2->cls, int_cls)) {
        BoxedInt* v2 = static_cast<BoxedInt*>(_v2);

        return boxBool(mpz_cmp_si(v1->n, v2->n) >= 0);
    } else {
        return NotImplemented;
    }
}

Box* longLt(BoxedLong* v1, Box* _v2) {
    if (!isSubclass(v1->cls, long_cls))
        raiseExcHelper(TypeError, "descriptor '__lt__' requires a 'long' object but received a '%s'",
                       getTypeName(v1)->c_str());

    if (isSubclass(_v2->cls, long_cls)) {
        BoxedLong* v2 = static_cast<BoxedLong*>(_v2);

        return boxBool(mpz_cmp(v1->n, v2->n) < 0);
    } else if (isSubclass(_v2->cls, int_cls)) {
        BoxedInt* v2 = static_cast<BoxedInt*>(_v2);

        return boxBool(mpz_cmp_si(v1->n, v2->n) < 0);
    } else {
        return NotImplemented;
    }
}

Box* longLe(BoxedLong* v1, Box* _v2) {
    if (!isSubclass(v1->cls, long_cls))
        raiseExcHelper(TypeError, "descriptor '__le__' requires a 'long' object but received a '%s'",
                       getTypeName(v1)->c_str());

    if (isSubclass(_v2->cls, long_cls)) {
        BoxedLong* v2 = static_cast<BoxedLong*>(_v2);

        return boxBool(mpz_cmp(v1->n, v2->n) <= 0);
    } else if (isSubclass(_v2->cls, int_cls)) {
        BoxedInt* v2 = static_cast<BoxedInt*>(_v2);

        return boxBool(mpz_cmp_si(v1->n, v2->n) <= 0);
    } else {
        return NotImplemented;
    }
}

Box* longEq(BoxedLong* v1, Box* _v2) {
    if (!isSubclass(v1->cls, long_cls))
        raiseExcHelper(TypeError, "descriptor '__eq__' requires a 'long' object but received a '%s'",
                       getTypeName(v1)->c_str());

    if (isSubclass(_v2->cls, long_cls)) {
        BoxedLong* v2 = static_cast<BoxedLong*>(_v2);

        return boxBool(mpz_cmp(v1->n, v2->n) == 0);
    } else if (isSubclass(_v2->cls, int_cls)) {
        BoxedInt* v2 = static_cast<BoxedInt*>(_v2);

        return boxBool(mpz_cmp_si(v1->n, v2->n) == 0);
    } else {
        return NotImplemented;
    }
}

Box* longNe(BoxedLong* v1, Box* _v2) {
    if (!isSubclass(v1->cls, long_cls))
        raiseExcHelper(TypeError, "descriptor '__ne__' requires a 'long' object but received a '%s'",
                       getTypeName(v1)->c_str());

    if (isSubclass(_v2->cls, long_cls)) {
        BoxedLong* v2 = static_cast<BoxedLong*>(_v2);

        return boxBool(mpz_cmp(v1->n, v2->n) != 0);
    } else if (isSubclass(_v2->cls, int_cls)) {
        BoxedInt* v2 = static_cast<BoxedInt*>(_v2);

        return boxBool(mpz_cmp_si(v1->n, v2->n) != 0);
    } else {
        return NotImplemented;
    }
}

Box* longLshift(BoxedLong* v1, Box* _v2) {
    if (!isSubclass(v1->cls, long_cls))
        raiseExcHelper(TypeError, "descriptor '__lshift__' requires a 'long' object but received a '%s'",
                       getTypeName(v1)->c_str());

    if (isSubclass(_v2->cls, long_cls)) {
        BoxedLong* v2 = static_cast<BoxedLong*>(_v2);

        if (mpz_cmp_si(v2->n, 0) < 0)
            raiseExcHelper(ValueError, "negative shift count");

        uint64_t n = asUnsignedLong(v2);
        BoxedLong* r = new BoxedLong(long_cls);
        mpz_init(r->n);
        mpz_mul_2exp(r->n, v1->n, n);
        return r;
    } else if (isSubclass(_v2->cls, int_cls)) {
        BoxedInt* v2 = static_cast<BoxedInt*>(_v2);
        if (v2->n < 0)
            raiseExcHelper(ValueError, "negative shift count");

        BoxedLong* r = new BoxedLong(long_cls);
        mpz_init(r->n);
        mpz_mul_2exp(r->n, v1->n, v2->n);
        return r;
    } else {
        return NotImplemented;
    }
}

Box* longRshift(BoxedLong* v1, Box* _v2) {
    if (!isSubclass(v1->cls, long_cls))
        raiseExcHelper(TypeError, "descriptor '__rshift__' requires a 'long' object but received a '%s'",
                       getTypeName(v1)->c_str());

    if (isSubclass(_v2->cls, long_cls)) {
        BoxedLong* v2 = static_cast<BoxedLong*>(_v2);

        if (mpz_cmp_si(v2->n, 0) < 0)
            raiseExcHelper(ValueError, "negative shift count");

        uint64_t n = asUnsignedLong(v2);
        BoxedLong* r = new BoxedLong(long_cls);
        mpz_init(r->n);
        mpz_div_2exp(r->n, v1->n, n);
        return r;
    } else if (isSubclass(_v2->cls, int_cls)) {
        BoxedInt* v2 = static_cast<BoxedInt*>(_v2);
        if (v2->n < 0)
            raiseExcHelper(ValueError, "negative shift count");

        BoxedLong* r = new BoxedLong(long_cls);
        mpz_init(r->n);
        mpz_div_2exp(r->n, v1->n, v2->n);
        return r;
    } else {
        return NotImplemented;
    }
}

Box* longSub(BoxedLong* v1, Box* _v2) {
    if (!isSubclass(v1->cls, long_cls))
        raiseExcHelper(TypeError, "descriptor '__sub__' requires a 'long' object but received a '%s'",
                       getTypeName(v1)->c_str());

    if (isSubclass(_v2->cls, long_cls)) {
        BoxedLong* v2 = static_cast<BoxedLong*>(_v2);

        BoxedLong* r = new BoxedLong(long_cls);
        mpz_init(r->n);
        mpz_sub(r->n, v1->n, v2->n);
        return r;
    } else if (isSubclass(_v2->cls, int_cls)) {
        BoxedInt* v2 = static_cast<BoxedInt*>(_v2);

        BoxedLong* r = new BoxedLong(long_cls);
        mpz_init(r->n);
        if (v2->n >= 0)
            mpz_sub_ui(r->n, v1->n, v2->n);
        else
            mpz_add_ui(r->n, v1->n, -v2->n);
        return r;
    } else {
        return NotImplemented;
    }
}

Box* longRsub(BoxedLong* v1, Box* _v2) {
    if (!isSubclass(v1->cls, long_cls))
        raiseExcHelper(TypeError, "descriptor '__rsub__' requires a 'long' object but received a '%s'",
                       getTypeName(v1)->c_str());

    return longAdd(static_cast<BoxedLong*>(longNeg(v1)), _v2);
}

Box* longMul(BoxedLong* v1, Box* _v2) {
    if (!isSubclass(v1->cls, long_cls))
        raiseExcHelper(TypeError, "descriptor '__mul__' requires a 'long' object but received a '%s'",
                       getTypeName(v1)->c_str());

    if (isSubclass(_v2->cls, long_cls)) {
        BoxedLong* v2 = static_cast<BoxedLong*>(_v2);

        BoxedLong* r = new BoxedLong(long_cls);
        mpz_init(r->n);
        mpz_mul(r->n, v1->n, v2->n);
        return r;
    } else if (isSubclass(_v2->cls, int_cls)) {
        BoxedInt* v2 = static_cast<BoxedInt*>(_v2);

        BoxedLong* r = new BoxedLong(long_cls);
        mpz_init(r->n);
        mpz_mul_si(r->n, v1->n, v2->n);
        return r;
    } else {
        return NotImplemented;
    }
}

Box* longDiv(BoxedLong* v1, Box* _v2) {
    if (!isSubclass(v1->cls, long_cls))
        raiseExcHelper(TypeError, "descriptor '__div__' requires a 'long' object but received a '%s'",
                       getTypeName(v1)->c_str());

    if (isSubclass(_v2->cls, long_cls)) {
        BoxedLong* v2 = static_cast<BoxedLong*>(_v2);

        if (mpz_cmp_si(v2->n, 0) == 0)
            raiseExcHelper(ZeroDivisionError, "long division or modulo by zero");

        BoxedLong* r = new BoxedLong(long_cls);
        mpz_init(r->n);
        mpz_fdiv_q(r->n, v1->n, v2->n);
        return r;
    } else if (isSubclass(_v2->cls, int_cls)) {
        BoxedInt* v2 = static_cast<BoxedInt*>(_v2);

        if (v2->n == 0)
            raiseExcHelper(ZeroDivisionError, "long division or modulo by zero");

        BoxedLong* r = new BoxedLong(long_cls);
        mpz_init_set_si(r->n, v2->n);
        mpz_fdiv_q(r->n, v1->n, r->n);
        return r;
    } else {
        return NotImplemented;
    }
}

extern "C" Box* longDivmod(BoxedLong* lhs, Box* _rhs) {
    if (!isSubclass(lhs->cls, long_cls))
        raiseExcHelper(TypeError, "descriptor '__div__' requires a 'long' object but received a '%s'",
                       getTypeName(lhs)->c_str());

    if (isSubclass(_rhs->cls, long_cls)) {
        BoxedLong* rhs = static_cast<BoxedLong*>(_rhs);

        if (mpz_cmp_si(rhs->n, 0) == 0)
            raiseExcHelper(ZeroDivisionError, "long division or modulo by zero");

        BoxedLong* q = new BoxedLong(long_cls);
        BoxedLong* r = new BoxedLong(long_cls);
        mpz_init(q->n);
        mpz_init(r->n);
        mpz_fdiv_qr(q->n, r->n, lhs->n, rhs->n);
        return new BoxedTuple({ q, r });
    } else if (isSubclass(_rhs->cls, int_cls)) {
        BoxedInt* rhs = static_cast<BoxedInt*>(_rhs);

        if (rhs->n == 0)
            raiseExcHelper(ZeroDivisionError, "long division or modulo by zero");

        BoxedLong* q = new BoxedLong(long_cls);
        BoxedLong* r = new BoxedLong(long_cls);
        mpz_init(q->n);
        mpz_init_set_si(r->n, rhs->n);
        mpz_fdiv_qr(q->n, r->n, lhs->n, r->n);
        return new BoxedTuple({ q, r });
    } else {
        return NotImplemented;
    }
}

Box* longRdiv(BoxedLong* v1, Box* _v2) {
    if (!isSubclass(v1->cls, long_cls))
        raiseExcHelper(TypeError, "descriptor '__div__' requires a 'long' object but received a '%s'",
                       getTypeName(v1)->c_str());

    if (mpz_cmp_si(v1->n, 0) == 0)
        raiseExcHelper(ZeroDivisionError, "long division or modulo by zero");

    if (isSubclass(_v2->cls, long_cls)) {
        BoxedLong* v2 = static_cast<BoxedLong*>(_v2);

        BoxedLong* r = new BoxedLong(long_cls);
        mpz_init(r->n);
        mpz_fdiv_q(r->n, v2->n, v1->n);
        return r;
    } else if (isSubclass(_v2->cls, int_cls)) {
        BoxedInt* v2 = static_cast<BoxedInt*>(_v2);

        BoxedLong* r = new BoxedLong(long_cls);
        mpz_init_set_si(r->n, v2->n);
        mpz_fdiv_q(r->n, r->n, v1->n);
        return r;
    } else {
        return NotImplemented;
    }
}

Box* longPow(BoxedLong* v1, Box* _v2) {
    if (!isSubclass(v1->cls, long_cls))
        raiseExcHelper(TypeError, "descriptor '__pow__' requires a 'long' object but received a '%s'",
                       getTypeName(v1)->c_str());

    if (!isSubclass(_v2->cls, long_cls))
        return NotImplemented;

    BoxedLong* v2 = static_cast<BoxedLong*>(_v2);

    RELEASE_ASSERT(mpz_sgn(v2->n) >= 0, "");
    RELEASE_ASSERT(mpz_fits_ulong_p(v2->n), "");
    uint64_t n2 = mpz_get_ui(v2->n);

    BoxedLong* r = new BoxedLong(long_cls);
    mpz_init(r->n);
    mpz_pow_ui(r->n, v1->n, n2);
    return r;
}

Box* longNonzero(BoxedLong* self) {
    if (!isSubclass(self->cls, long_cls))
        raiseExcHelper(TypeError, "descriptor '__pow__' requires a 'long' object but received a '%s'",
                       getTypeName(self)->c_str());

    if (mpz_cmp_si(self->n, 0) == 0)
        return False;
    return True;
}

Box* longHash(BoxedLong* self) {
    if (!isSubclass(self->cls, long_cls))
        raiseExcHelper(TypeError, "descriptor '__pow__' requires a 'long' object but received a '%s'",
                       getTypeName(self)->c_str());

    // Not sure if this is a good hash function or not;
    // simple, but only includes top bits:
    union {
        uint64_t n;
        double d;
    };
    d = mpz_get_d(self->n);
    return boxInt(n);
}

void setupLong() {
    long_cls->giveAttr("__name__", boxStrConstant("long"));

    long_cls->giveAttr(
        "__new__", new BoxedFunction(boxRTFunction((void*)longNew, UNKNOWN, 3, 2, false, false), { boxInt(0), NULL }));

    long_cls->giveAttr("__mul__", new BoxedFunction(boxRTFunction((void*)longMul, UNKNOWN, 2)));
    long_cls->giveAttr("__rmul__", long_cls->getattr("__mul__"));

    long_cls->giveAttr("__div__", new BoxedFunction(boxRTFunction((void*)longDiv, UNKNOWN, 2)));
    long_cls->giveAttr("__rdiv__", new BoxedFunction(boxRTFunction((void*)longRdiv, UNKNOWN, 2)));

    long_cls->giveAttr("__divmod__", new BoxedFunction(boxRTFunction((void*)longDivmod, UNKNOWN, 2)));

    long_cls->giveAttr("__sub__", new BoxedFunction(boxRTFunction((void*)longSub, UNKNOWN, 2)));
    long_cls->giveAttr("__rsub__", new BoxedFunction(boxRTFunction((void*)longRsub, UNKNOWN, 2)));

    long_cls->giveAttr("__add__", new BoxedFunction(boxRTFunction((void*)longAdd, UNKNOWN, 2)));
    long_cls->giveAttr("__radd__", long_cls->getattr("__add__"));

    long_cls->giveAttr("__and__", new BoxedFunction(boxRTFunction((void*)longAnd, UNKNOWN, 2)));
    long_cls->giveAttr("__rand__", long_cls->getattr("__and__"));
    long_cls->giveAttr("__xor__", new BoxedFunction(boxRTFunction((void*)longXor, UNKNOWN, 2)));
    long_cls->giveAttr("__rxor__", long_cls->getattr("__xor__"));

    long_cls->giveAttr("__gt__", new BoxedFunction(boxRTFunction((void*)longGt, UNKNOWN, 2)));
    long_cls->giveAttr("__ge__", new BoxedFunction(boxRTFunction((void*)longGe, UNKNOWN, 2)));
    long_cls->giveAttr("__lt__", new BoxedFunction(boxRTFunction((void*)longLt, UNKNOWN, 2)));
    long_cls->giveAttr("__le__", new BoxedFunction(boxRTFunction((void*)longLe, UNKNOWN, 2)));
    long_cls->giveAttr("__eq__", new BoxedFunction(boxRTFunction((void*)longEq, UNKNOWN, 2)));
    long_cls->giveAttr("__ne__", new BoxedFunction(boxRTFunction((void*)longNe, UNKNOWN, 2)));

    long_cls->giveAttr("__lshift__", new BoxedFunction(boxRTFunction((void*)longLshift, UNKNOWN, 2)));
    long_cls->giveAttr("__rshift__", new BoxedFunction(boxRTFunction((void*)longRshift, UNKNOWN, 2)));

    long_cls->giveAttr("__repr__", new BoxedFunction(boxRTFunction((void*)longRepr, STR, 1)));
    long_cls->giveAttr("__str__", new BoxedFunction(boxRTFunction((void*)longStr, STR, 1)));

    long_cls->giveAttr("__nonzero__", new BoxedFunction(boxRTFunction((void*)longNonzero, BOXED_BOOL, 1)));
    long_cls->giveAttr("__hash__", new BoxedFunction(boxRTFunction((void*)longHash, BOXED_INT, 1)));

    long_cls->freeze();
}
}
