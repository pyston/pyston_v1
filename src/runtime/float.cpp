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

#include <cfloat>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <gmp.h>

#include "capi/typeobject.h"
#include "capi/types.h"
#include "core/types.h"
#include "runtime/inline/boxing.h"
#include "runtime/long.h"
#include "runtime/objmodel.h"
#include "runtime/types.h"
#include "runtime/util.h"

extern "C" int float_coerce(PyObject** v, PyObject** w) noexcept;
extern "C" int float_pow_unboxed(double iv, double iw, double* res) noexcept;
extern "C" PyObject* float_as_integer_ratio(PyObject* v, PyObject* unused) noexcept;
extern "C" PyObject* float_divmod(PyObject* v, PyObject* w) noexcept;
extern "C" PyObject* float__format__(PyObject* v) noexcept;
extern "C" PyObject* float_fromhex(PyObject* cls, PyObject* arg) noexcept;
extern "C" PyObject* float_getformat(PyTypeObject* v, PyObject* arg) noexcept;
extern "C" PyObject* float_getnewargs(PyFloatObject* v) noexcept;
extern "C" PyObject* float_hex(PyObject* v) noexcept;
extern "C" PyObject* float_is_integer(PyObject* v) noexcept;
extern "C" PyObject* float_pow(PyObject* v, PyObject* w, PyObject* z) noexcept;
extern "C" PyObject* float_richcompare(PyObject* v, PyObject* w, int op) noexcept;
extern "C" PyObject* float_setformat(PyTypeObject* v, PyObject* args) noexcept;
extern "C" PyObject* float_str(PyObject* v) noexcept;

namespace pyston {

/* Special free list -- see comments for same code in intobject.c. */
#define BLOCK_SIZE 1000 /* 1K less typical malloc overhead */
#define BHEAD_SIZE 8    /* Enough for a 64-bit pointer */
#define N_FLOATOBJECTS ((BLOCK_SIZE - BHEAD_SIZE) / sizeof(PyFloatObject))

struct _floatblock {
    struct _floatblock* next;
    PyFloatObject objects[N_FLOATOBJECTS];
};

typedef struct _floatblock PyFloatBlock;

static PyFloatBlock* block_list = NULL;
PyFloatObject* BoxedFloat::free_list = NULL;

PyFloatObject* BoxedFloat::fill_free_list(void) noexcept {
    PyFloatObject* p, *q;
    /* XXX Float blocks escape the object heap. Use PyObject_MALLOC ??? */
    p = (PyFloatObject*)PyMem_MALLOC(sizeof(PyFloatBlock));
    if (p == NULL)
        return (PyFloatObject*)PyErr_NoMemory();
    ((PyFloatBlock*)p)->next = block_list;
    block_list = (PyFloatBlock*)p;
    p = &((PyFloatBlock*)p)->objects[0];
    q = p + N_FLOATOBJECTS;
    while (--q > p)
        q->ob_type = (struct _typeobject*)(q - 1);
    q->ob_type = NULL;
    return p + N_FLOATOBJECTS - 1;
}

void BoxedFloat::tp_dealloc(Box* b) noexcept {
#ifdef DISABLE_INT_FREELIST
    b->cls->tp_free(b);
#else
    if (likely(PyFloat_CheckExact(b))) {
        PyFloatObject* v = (PyFloatObject*)(b);
        v->ob_type = (struct _typeobject*)free_list;
        free_list = v;
    } else {
        b->cls->tp_free(b);
    }
#endif
}

extern "C" int PyFloat_ClearFreeList() noexcept {
    PyFloatObject* p;
    PyFloatBlock* list, *next;
    int i;
    int u; /* remaining unfreed ints per block */
    int freelist_size = 0;

    list = block_list;
    block_list = NULL;
    BoxedFloat::free_list = NULL;
    while (list != NULL) {
        u = 0;
        for (i = 0, p = &list->objects[0]; i < N_FLOATOBJECTS; i++, p++) {
            if (PyFloat_CheckExact((BoxedFloat*)p) && Py_REFCNT(p) != 0)
                u++;
        }
        next = list->next;
        if (u) {
            list->next = block_list;
            block_list = list;
            for (i = 0, p = &list->objects[0]; i < N_FLOATOBJECTS; i++, p++) {
                if (!PyFloat_CheckExact((BoxedFloat*)p) || Py_REFCNT(p) == 0) {
                    p->ob_type = (struct _typeobject*)BoxedFloat::free_list;
                    BoxedFloat::free_list = p;
                }
            }
        } else {
            PyMem_FREE(list);
        }
        freelist_size += u;
        list = next;
    }
    return freelist_size;
}

extern "C" PyObject* PyFloat_FromDouble(double d) noexcept {
    return boxFloat(d);
}

extern "C" double PyFloat_AsDouble(PyObject* o) noexcept {
    assert(o);

    if (PyFloat_CheckExact(o))
        return static_cast<BoxedFloat*>(o)->d;

    // special case: int (avoids all the boxing below, and int_cls->tp_as_number
    // isn't implemented at the time of this writing)
    // This is an exact check.
    if (o->cls == int_cls || o->cls == bool_cls)
        return (double)static_cast<BoxedInt*>(o)->n;
    // special case: long
    if (o->cls == long_cls) {
        double result = PyLong_AsDouble(static_cast<BoxedLong*>(o));
        if (result == -1.0 && PyErr_Occurred())
            return -1.0;

        return result;
    }

    // implementation from cpython:
    PyNumberMethods* nb;
    BoxedFloat* fo;
    double val;

    if ((nb = Py_TYPE(o)->tp_as_number) == NULL || nb->nb_float == NULL) {
        PyErr_SetString(PyExc_TypeError, "a float is required");
        return -1;
    };
    fo = (BoxedFloat*)(*nb->nb_float)(o);
    if (fo == NULL)
        return -1;
    AUTO_DECREF(fo);
    if (!PyFloat_Check(fo)) {
        PyErr_SetString(PyExc_TypeError, "nb_float should return float object");
        return -1;
    }

    val = static_cast<BoxedFloat*>(fo)->d;

    return val;
}

template <typename T> static inline void raiseDivZeroExcIfZero(T var) {
    if (var == 0) {
        raiseExcHelper(ZeroDivisionError, "float division by zero");
    }
}

extern "C" double mod_float_float(double lhs, double rhs) {
    raiseDivZeroExcIfZero(rhs);
    double r = fmod(lhs, rhs);
    // Have to be careful here with signed zeroes:
    if (std::signbit(r) != std::signbit(rhs)) {
        if (r == 0)
            r *= -1;
        else
            r += rhs;
    }
    return r;
}

extern "C" double div_float_float(double lhs, double rhs) {
    raiseDivZeroExcIfZero(rhs);
    return lhs / rhs;
}

extern "C" double floordiv_float_float(double lhs, double rhs) {
    raiseDivZeroExcIfZero(rhs);
    return floor(lhs / rhs);
}

extern "C" Box* floatAddFloat(BoxedFloat* lhs, BoxedFloat* rhs) {
    assert(PyFloat_Check(lhs));
    assert(PyFloat_Check(rhs));
    return boxFloat(lhs->d + rhs->d);
}

extern "C" Box* floatAddInt(BoxedFloat* lhs, BoxedInt* rhs) {
    assert(PyFloat_Check(lhs));
    assert(PyInt_Check(rhs));
    return boxFloat(lhs->d + rhs->n);
}

extern "C" Box* floatAdd(BoxedFloat* lhs, Box* rhs) {
    assert(PyFloat_Check(lhs));
    if (PyInt_Check(rhs)) {
        return floatAddInt(lhs, static_cast<BoxedInt*>(rhs));
    } else if (PyFloat_Check(rhs)) {
        return floatAddFloat(lhs, static_cast<BoxedFloat*>(rhs));
    } else if (PyLong_Check(rhs)) {
        double rhs_f = PyLong_AsDouble(rhs);
        if (rhs_f == -1.0 && PyErr_Occurred()) {
            throwCAPIException();
        }
        return boxFloat(lhs->d + rhs_f);
    } else {
        return incref(NotImplemented);
    }
}

extern "C" Box* floatDivFloat(BoxedFloat* lhs, BoxedFloat* rhs) {
    assert(PyFloat_Check(lhs));
    assert(PyFloat_Check(rhs));
    raiseDivZeroExcIfZero(rhs->d);
    return boxFloat(lhs->d / rhs->d);
}

extern "C" Box* floatDivInt(BoxedFloat* lhs, BoxedInt* rhs) {
    assert(PyFloat_Check(lhs));
    assert(PyInt_Check(rhs));
    raiseDivZeroExcIfZero(rhs->n);
    return boxFloat(lhs->d / rhs->n);
}

extern "C" Box* floatDiv(BoxedFloat* lhs, Box* rhs) {
    assert(PyFloat_Check(lhs));
    if (PyInt_Check(rhs)) {
        return floatDivInt(lhs, static_cast<BoxedInt*>(rhs));
    } else if (PyFloat_Check(rhs)) {
        return floatDivFloat(lhs, static_cast<BoxedFloat*>(rhs));
    } else if (PyLong_Check(rhs)) {
        double rhs_f = PyLong_AsDouble(rhs);
        if (rhs_f == -1.0 && PyErr_Occurred()) {
            throwCAPIException();
        }
        return boxFloat(lhs->d / rhs_f);
    } else {
        return incref(NotImplemented);
    }
}

extern "C" Box* floatTruediv(BoxedFloat* lhs, Box* rhs) {
    assert(PyFloat_Check(lhs));
    if (PyInt_Check(rhs)) {
        return floatDivInt(lhs, static_cast<BoxedInt*>(rhs));
    } else if (PyFloat_Check(rhs)) {
        return floatDivFloat(lhs, static_cast<BoxedFloat*>(rhs));
    } else if (PyLong_Check(rhs)) {
        double rhs_f = PyLong_AsDouble(rhs);
        if (rhs_f == -1.0 && PyErr_Occurred()) {
            throwCAPIException();
        }
        return boxFloat(lhs->d / rhs_f);
    } else {
        return incref(NotImplemented);
    }
}

extern "C" Box* floatRDivFloat(BoxedFloat* lhs, BoxedFloat* rhs) {
    assert(PyFloat_Check(lhs));
    assert(PyFloat_Check(rhs));
    raiseDivZeroExcIfZero(lhs->d);
    return boxFloat(rhs->d / lhs->d);
}

extern "C" Box* floatRDivInt(BoxedFloat* lhs, BoxedInt* rhs) {
    assert(PyFloat_Check(lhs));
    assert(PyInt_Check(rhs));
    raiseDivZeroExcIfZero(lhs->d);
    return boxFloat(rhs->n / lhs->d);
}

extern "C" Box* floatRDiv(BoxedFloat* lhs, Box* rhs) {
    assert(PyFloat_Check(lhs));
    if (PyInt_Check(rhs)) {
        return floatRDivInt(lhs, static_cast<BoxedInt*>(rhs));
    } else if (PyFloat_Check(rhs)) {
        return floatRDivFloat(lhs, static_cast<BoxedFloat*>(rhs));
    } else if (PyLong_Check(rhs)) {
        double rhs_f = PyLong_AsDouble(rhs);
        if (rhs_f == -1.0 && PyErr_Occurred()) {
            throwCAPIException();
        }
        raiseDivZeroExcIfZero(lhs->d);
        return boxFloat(rhs_f / lhs->d);
    } else {
        return incref(NotImplemented);
    }
}

Box* floatRTruediv(BoxedFloat* lhs, Box* rhs) {
    if (!PyFloat_Check(lhs))
        raiseExcHelper(TypeError, "descriptor '__rtruediv__' requires a 'float' object but received a '%s'",
                       getTypeName(lhs));

    if (PyInt_Check(rhs)) {
        return floatRDivInt(lhs, static_cast<BoxedInt*>(rhs));
    } else if (PyFloat_Check(rhs)) {
        return floatRDivFloat(lhs, static_cast<BoxedFloat*>(rhs));
    } else if (PyLong_Check(rhs)) {
        double rhs_f = PyLong_AsDouble(rhs);
        if (rhs_f == -1.0 && PyErr_Occurred()) {
            throwCAPIException();
        }
        raiseDivZeroExcIfZero(lhs->d);
        return boxFloat(rhs_f / lhs->d);
    } else {
        return incref(NotImplemented);
    }
}

extern "C" Box* floatFloorDivFloat(BoxedFloat* lhs, BoxedFloat* rhs) {
    assert(PyFloat_Check(lhs));
    assert(PyFloat_Check(rhs));
    raiseDivZeroExcIfZero(rhs->d);
    return boxFloat(floor(lhs->d / rhs->d));
}

extern "C" Box* floatFloorDivInt(BoxedFloat* lhs, BoxedInt* rhs) {
    assert(PyFloat_Check(lhs));
    assert(PyInt_Check(rhs));
    raiseDivZeroExcIfZero(rhs->n);
    return boxFloat(floor(lhs->d / rhs->n));
}

extern "C" Box* floatFloorDiv(BoxedFloat* lhs, Box* rhs) {
    assert(PyFloat_Check(lhs));
    if (PyInt_Check(rhs)) {
        return floatFloorDivInt(lhs, static_cast<BoxedInt*>(rhs));
    } else if (PyFloat_Check(rhs)) {
        return floatFloorDivFloat(lhs, static_cast<BoxedFloat*>(rhs));
    } else if (PyLong_Check(rhs)) {
        double rhs_f = PyLong_AsDouble(rhs);
        if (rhs_f == -1.0 && PyErr_Occurred()) {
            throwCAPIException();
        }
        return floatFloorDivFloat(lhs, new BoxedFloat(rhs_f));
    } else {
        return incref(NotImplemented);
    }
}

extern "C" Box* floatRFloorDiv(BoxedFloat* lhs, Box* _rhs) {
    assert(PyFloat_Check(lhs));
    if (PyInt_Check(_rhs)) {
        BoxedInt* rhs = (BoxedInt*)_rhs;
        raiseDivZeroExcIfZero(lhs->d);
        return boxFloat(floor(rhs->n / lhs->d));
    } else if (PyFloat_Check(_rhs)) {
        BoxedFloat* rhs = (BoxedFloat*)_rhs;
        raiseDivZeroExcIfZero(lhs->d);
        return boxFloat(floor(rhs->d / lhs->d));
    } else if (PyLong_Check(_rhs)) {
        double rhs_f = PyLong_AsDouble(_rhs);
        if (rhs_f == -1.0 && PyErr_Occurred()) {
            throwCAPIException();
        }
        return floatFloorDivFloat(new BoxedFloat(rhs_f), lhs);
    } else {
        return incref(NotImplemented);
    }
}

extern "C" Box* floatEq(BoxedFloat* lhs, Box* rhs) {
    if (!PyFloat_Check(lhs)) {
        raiseExcHelper(TypeError, "descriptor '__eq__' requires a 'float' object but received a '%s'",
                       getTypeName(lhs));
    }
    Box* res = float_richcompare(lhs, rhs, Py_EQ);
    if (!res) {
        throwCAPIException();
    }

    return res;
}

extern "C" Box* floatNe(BoxedFloat* lhs, Box* rhs) {
    if (!PyFloat_Check(lhs)) {
        raiseExcHelper(TypeError, "descriptor '__ne__' requires a 'float' object but received a '%s'",
                       getTypeName(lhs));
    }
    Box* res = float_richcompare(lhs, rhs, Py_NE);
    if (!res) {
        throwCAPIException();
    }

    return res;
}

extern "C" Box* floatLe(BoxedFloat* lhs, Box* rhs) {
    if (!PyFloat_Check(lhs)) {
        raiseExcHelper(TypeError, "descriptor '__le__' requires a 'float' object but received a '%s'",
                       getTypeName(lhs));
    }
    Box* res = float_richcompare(lhs, rhs, Py_LE);
    if (!res) {
        throwCAPIException();
    }

    return res;
}

extern "C" Box* floatLt(BoxedFloat* lhs, Box* rhs) {
    if (!PyFloat_Check(lhs)) {
        raiseExcHelper(TypeError, "descriptor '__lt__' requires a 'float' object but received a '%s'",
                       getTypeName(lhs));
    }
    Box* res = float_richcompare(lhs, rhs, Py_LT);
    if (!res) {
        throwCAPIException();
    }

    return res;
}

extern "C" Box* floatGe(BoxedFloat* lhs, Box* rhs) {
    if (!PyFloat_Check(lhs)) {
        raiseExcHelper(TypeError, "descriptor '__ge__' requires a 'float' object but received a '%s'",
                       getTypeName(lhs));
    }
    Box* res = float_richcompare(lhs, rhs, Py_GE);
    if (!res) {
        throwCAPIException();
    }

    return res;
}

extern "C" Box* floatGt(BoxedFloat* lhs, Box* rhs) {
    if (!PyFloat_Check(lhs)) {
        raiseExcHelper(TypeError, "descriptor '__gt__' requires a 'float' object but received a '%s'",
                       getTypeName(lhs));
    }
    Box* res = float_richcompare(lhs, rhs, Py_GT);
    if (!res) {
        throwCAPIException();
    }

    return res;
}

extern "C" Box* floatModFloat(BoxedFloat* lhs, BoxedFloat* rhs) {
    assert(PyFloat_Check(lhs));
    assert(PyFloat_Check(rhs));
    return boxFloat(mod_float_float(lhs->d, rhs->d));
}

extern "C" Box* floatModInt(BoxedFloat* lhs, BoxedInt* rhs) {
    assert(PyFloat_Check(lhs));
    assert(PyInt_Check(rhs));
    return boxFloat(mod_float_float(lhs->d, rhs->n));
}

extern "C" Box* floatMod(BoxedFloat* lhs, Box* rhs) {
    assert(PyFloat_Check(lhs));
    if (PyInt_Check(rhs)) {
        return floatModInt(lhs, static_cast<BoxedInt*>(rhs));
    } else if (PyFloat_Check(rhs)) {
        return floatModFloat(lhs, static_cast<BoxedFloat*>(rhs));
    } else if (PyLong_Check(rhs)) {
        double rhs_f = PyLong_AsDouble(rhs);
        if (rhs_f == -1.0 && PyErr_Occurred()) {
            throwCAPIException();
        }
        return boxFloat(mod_float_float(lhs->d, rhs_f));
    } else {
        return incref(NotImplemented);
    }
}

extern "C" Box* floatRModFloat(BoxedFloat* lhs, BoxedFloat* rhs) {
    assert(PyFloat_Check(lhs));
    assert(PyFloat_Check(rhs));
    return boxFloat(mod_float_float(rhs->d, lhs->d));
}

extern "C" Box* floatRModInt(BoxedFloat* lhs, BoxedInt* rhs) {
    assert(PyFloat_Check(lhs));
    assert(PyInt_Check(rhs));
    return boxFloat(mod_float_float(rhs->n, lhs->d));
}

extern "C" Box* floatRMod(BoxedFloat* lhs, Box* rhs) {
    assert(PyFloat_Check(lhs));
    if (PyInt_Check(rhs)) {
        return floatRModInt(lhs, static_cast<BoxedInt*>(rhs));
    } else if (PyFloat_Check(rhs)) {
        return floatRModFloat(lhs, static_cast<BoxedFloat*>(rhs));
    } else if (PyLong_Check(rhs)) {
        double rhs_f = PyLong_AsDouble(rhs);
        if (rhs_f == -1.0 && PyErr_Occurred()) {
            throwCAPIException();
        }
        return boxFloat(mod_float_float(rhs_f, lhs->d));
    } else {
        return incref(NotImplemented);
    }
}

Box* floatDivmod(BoxedFloat* lhs, Box* rhs) {
    if (!PyFloat_Check(lhs))
        raiseExcHelper(TypeError, "descriptor '__divmod__' requires a 'float' object but received a '%s'",
                       getTypeName(lhs));

    Box* res = float_divmod(lhs, rhs);
    if (!res) {
        throwCAPIException();
    }
    return res;
}

Box* floatRDivmod(BoxedFloat* lhs, Box* rhs) {
    if (!PyFloat_Check(lhs))
        raiseExcHelper(TypeError, "descriptor '__rdivmod__' requires a 'float' object but received a '%s'",
                       getTypeName(lhs));

    Box* res = float_divmod(rhs, lhs);
    if (!res) {
        throwCAPIException();
    }
    return res;
}

extern "C" Box* floatPow(BoxedFloat* lhs, Box* rhs, Box* mod) {
    Box* res = float_pow(lhs, rhs, mod);
    if (!res) {
        throwCAPIException();
    }
    return res;
}

extern "C" Box* floatPowFloat(BoxedFloat* lhs, BoxedFloat* rhs, Box* mod = None) {
    // TODO to specialize this, need to account for all the special cases in float_pow
    assert(PyFloat_Check(lhs));
    assert(PyFloat_Check(rhs));
    return floatPow(lhs, rhs, mod);
}

extern "C" Box* floatPowInt(BoxedFloat* lhs, BoxedInt* rhs, Box* mod = None) {
    // TODO to specialize this, need to account for all the special cases in float_pow
    assert(PyFloat_Check(lhs));
    assert(PyInt_Check(rhs));
    return floatPow(lhs, rhs, mod);
}

Box* floatRPow(BoxedFloat* lhs, Box* rhs) {
    if (!PyFloat_Check(lhs))
        raiseExcHelper(TypeError, "descriptor '__rpow__' requires a 'float' object but received a '%s'",
                       getTypeName(lhs));

    Box* res = float_pow(rhs, lhs, None);
    if (!res) {
        throwCAPIException();
    }
    return res;
}

extern "C" double pow_float_float(double lhs, double rhs) {
    double res;
    int err = float_pow_unboxed(lhs, rhs, &res);
    if (err) {
        throwCAPIException();
    } else {
        return res;
    }
}

extern "C" Box* floatMulFloat(BoxedFloat* lhs, BoxedFloat* rhs) {
    assert(PyFloat_Check(lhs));
    assert(PyFloat_Check(rhs));
    return boxFloat(lhs->d * rhs->d);
}

extern "C" Box* floatMulInt(BoxedFloat* lhs, BoxedInt* rhs) {
    assert(PyFloat_Check(lhs));
    assert(PyInt_Check(rhs));
    return boxFloat(lhs->d * rhs->n);
}

extern "C" Box* floatMul(BoxedFloat* lhs, Box* rhs) {
    assert(PyFloat_Check(lhs));
    if (PyInt_Check(rhs)) {
        return floatMulInt(lhs, static_cast<BoxedInt*>(rhs));
    } else if (PyFloat_Check(rhs)) {
        return floatMulFloat(lhs, static_cast<BoxedFloat*>(rhs));
    } else if (PyLong_Check(rhs)) {
        double rhs_f = PyLong_AsDouble(rhs);
        if (rhs_f == -1.0 && PyErr_Occurred()) {
            throwCAPIException();
        }
        return boxFloat(lhs->d * rhs_f);
    } else {
        return incref(NotImplemented);
    }
}

extern "C" Box* floatSubFloat(BoxedFloat* lhs, BoxedFloat* rhs) {
    assert(PyFloat_Check(lhs));
    assert(PyFloat_Check(rhs));
    return boxFloat(lhs->d - rhs->d);
}

extern "C" Box* floatSubInt(BoxedFloat* lhs, BoxedInt* rhs) {
    assert(PyFloat_Check(lhs));
    assert(PyInt_Check(rhs));
    return boxFloat(lhs->d - rhs->n);
}

extern "C" Box* floatSub(BoxedFloat* lhs, Box* rhs) {
    assert(PyFloat_Check(lhs));
    if (PyInt_Check(rhs)) {
        return floatSubInt(lhs, static_cast<BoxedInt*>(rhs));
    } else if (PyFloat_Check(rhs)) {
        return floatSubFloat(lhs, static_cast<BoxedFloat*>(rhs));
    } else if (PyLong_Check(rhs)) {
        double rhs_f = PyLong_AsDouble(rhs);
        if (rhs_f == -1.0 && PyErr_Occurred()) {
            throwCAPIException();
        }
        return boxFloat(lhs->d - rhs_f);
    } else {
        return incref(NotImplemented);
    }
}

extern "C" Box* floatRSubFloat(BoxedFloat* lhs, BoxedFloat* rhs) {
    assert(PyFloat_Check(lhs));
    assert(PyFloat_Check(rhs));
    return boxFloat(rhs->d - lhs->d);
}

extern "C" Box* floatRSubInt(BoxedFloat* lhs, BoxedInt* rhs) {
    assert(PyFloat_Check(lhs));
    assert(PyInt_Check(rhs));
    return boxFloat(rhs->n - lhs->d);
}

extern "C" Box* floatRSub(BoxedFloat* lhs, Box* rhs) {
    assert(PyFloat_Check(lhs));
    if (PyInt_Check(rhs)) {
        return floatRSubInt(lhs, static_cast<BoxedInt*>(rhs));
    } else if (PyFloat_Check(rhs)) {
        return floatRSubFloat(lhs, static_cast<BoxedFloat*>(rhs));
    } else if (PyLong_Check(rhs)) {
        double rhs_f = PyLong_AsDouble(rhs);
        if (rhs_f == -1.0 && PyErr_Occurred()) {
            throwCAPIException();
        }
        return boxFloat(rhs_f - lhs->d);
    } else {
        return incref(NotImplemented);
    }
}

Box* floatNeg(BoxedFloat* self) {
    assert(PyFloat_Check(self));
    return boxFloat(-self->d);
}

Box* floatPos(BoxedFloat* self) {
    assert(PyFloat_Check(self));
    return PyFloat_FromDouble(self->d);
}

Box* floatAbs(BoxedFloat* self) {
    if (!PyFloat_Check(self))
        raiseExcHelper(TypeError, "descriptor '__abs__' requires a 'float' object but received a '%s'",
                       getTypeName(self));
    double res = std::abs(self->d);
    return boxFloat(res);
}

bool floatNonzeroUnboxed(BoxedFloat* self) {
    assert(PyFloat_Check(self));
    return self->d != 0.0;
}

Box* floatNonzero(BoxedFloat* self) {
    return boxBool(floatNonzeroUnboxed(self));
}

template <ExceptionStyle S> static BoxedFloat* _floatNew(Box* x) noexcept(S == CAPI) {
    Box* rtn;
    if (PyString_CheckExact(x))
        rtn = PyFloat_FromString(x, NULL);
    else
        rtn = PyNumber_Float(x);
    if (!rtn && S == CXX)
        throwCAPIException();
    return static_cast<BoxedFloat*>(rtn);
}

template <ExceptionStyle S> Box* floatNew(BoxedClass* _cls, Box* a) noexcept(S == CAPI) {
    if (!PyType_Check(_cls)) {
        if (S == CAPI) {
            PyErr_Format(TypeError, "float.__new__(X): X is not a type object (%s)", getTypeName(_cls));
            return NULL;
        } else
            raiseExcHelper(TypeError, "float.__new__(X): X is not a type object (%s)", getTypeName(_cls));
    }

    BoxedClass* cls = static_cast<BoxedClass*>(_cls);
    if (!isSubclass(cls, float_cls)) {
        if (S == CAPI) {
            PyErr_Format(TypeError, "float.__new__(%s): %s is not a subtype of float", getNameOfClass(cls),
                         getNameOfClass(cls));
            return NULL;
        } else {
            raiseExcHelper(TypeError, "float.__new__(%s): %s is not a subtype of float", getNameOfClass(cls),
                           getNameOfClass(cls));
        }
    }

    if (cls == float_cls)
        return _floatNew<S>(a);

    BoxedFloat* f = _floatNew<S>(a);
    if (!f) {
        assert(S == CAPI);
        return NULL;
    }
    AUTO_DECREF(f);

    return new (cls) BoxedFloat(f->d);
}

// Roughly analogous to CPython's float_new.
// The arguments need to be unpacked from args and kwds.
static Box* floatNewPacked(BoxedClass* type, Box* args, Box* kwds) noexcept {
    PyObject* x
        = Py_False; // False is like initalizing it to 0.0 but faster because we don't need to box it in case the
                    // optional arg exists
    static char* kwlist[2] = { NULL, NULL };
    kwlist[0] = const_cast<char*>("x");

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|O:float", kwlist, &x))
        return NULL;

    return floatNew<CAPI>(type, x);
}

PyObject* float_str_or_repr(double v, int precision, char format_code) {
    PyObject* result;
    char* buf = PyOS_double_to_string(v, format_code, precision, Py_DTSF_ADD_DOT_0, NULL);
    if (!buf)
        return PyErr_NoMemory();
    result = PyString_FromString(buf);
    PyMem_Free(buf);
    return result;
}

extern "C" Box* floatFloat(BoxedFloat* self) {
    if (!PyFloat_Check(self))
        raiseExcHelper(TypeError, "descriptor '__float__' requires a 'float' object but received a '%s'",
                       getTypeName(self));

    if (self->cls == float_cls)
        return incref(self);
    return boxFloat(self->d);
}

template <ExceptionStyle S> Box* floatStr(BoxedFloat* self) noexcept(S == CAPI) {
    if (!PyFloat_Check(self))
        return setDescrTypeError<S>(self, "float", "__str__");
    return callCAPIFromStyle<S>(float_str_or_repr, self->d, PyFloat_STR_PRECISION, 'g');
}

template <ExceptionStyle S> Box* floatRepr(BoxedFloat* self) noexcept(S == CAPI) {
    if (!PyFloat_Check(self))
        return setDescrTypeError<S>(self, "float", "__repr__");
    return callCAPIFromStyle<S>(float_str_or_repr, self->d, 0, 'r');
}

Box* float_to_int(BoxedFloat* self) noexcept {
    double wholepart; /* integral portion of x, rounded toward 0 */

    (void)modf(self->d, &wholepart);
    /* Try to get out cheap if this fits in a Python int.  The attempt
     * to cast to long must be protected, as C doesn't define what
     * happens if the double is too big to fit in a long.  Some rare
     * systems raise an exception then (RISCOS was mentioned as one,
     * and someone using a non-default option on Sun also bumped into
     * that).  Note that checking for <= LONG_MAX is unsafe: if a long
     * has more bits of precision than a double, casting LONG_MAX to
     * double may yield an approximation, and if that's rounded up,
     * then, e.g., wholepart=LONG_MAX+1 would yield true from the C
     * expression wholepart<=LONG_MAX, despite that wholepart is
     * actually greater than LONG_MAX.  However, assuming a two's complement
     * machine with no trap representation, LONG_MIN will be a power of 2 (and
     * hence exactly representable as a double), and LONG_MAX = -1-LONG_MIN, so
     * the comparisons with (double)LONG_MIN below should be safe.
     */
    if ((double)LONG_MIN <= wholepart && wholepart < -(double)LONG_MIN) {
        const long aslong = (long)wholepart;
        return PyInt_FromLong(aslong);
    }
    return PyLong_FromDouble(wholepart);
}

Box* floatTrunc(BoxedFloat* self) {
    if (!PyFloat_Check(self))
        raiseExcHelper(TypeError, "descriptor '__trunc__' requires a 'float' object but received a '%s'",
                       getTypeName(self));

    return float_to_int(self);
}

Box* floatInt(BoxedFloat* self) {
    if (!PyFloat_Check(self))
        raiseExcHelper(TypeError, "descriptor '__int__' requires a 'float' object but received a '%s'",
                       getTypeName(self));
    return float_to_int(self);
}

Box* floatLong(BoxedFloat* self) {
    if (!PyFloat_Check(self))
        raiseExcHelper(TypeError, "descriptor '__long__' requires a 'float' object but received a '%s'",
                       getTypeName(self));

    double x = PyFloat_AsDouble(self);
    return PyLong_FromDouble(x);
}

Box* floatCoerce(BoxedFloat* _self, Box* other) {
    if (!PyFloat_Check(_self))
        raiseExcHelper(TypeError, "descriptor '__coerce__' requires a 'float' object but received a '%s'",
                       getTypeName(_self));

    Box* self = static_cast<Box*>(_self);
    int result = float_coerce(&self, &other);
    if (result == 0) {
        AUTO_DECREF(self);
        AUTO_DECREF(other);
        return BoxedTuple::create({ self, other });
    } else if (result == 1)
        return incref(NotImplemented);
    else
        throwCAPIException();
}

Box* floatHash(BoxedFloat* self) {
    if (!PyFloat_Check(self))
        raiseExcHelper(TypeError, "descriptor '__hash__' requires a 'float' object but received a '%s'",
                       getTypeName(self));

    return boxInt(_Py_HashDouble(self->d));
}

static void _addFunc(const char* name, ConcreteCompilerType* rtn_type, void* float_func, void* int_func,
                     void* boxed_func) {
    std::vector<ConcreteCompilerType*> v_ff, v_fi, v_uu;
    v_ff.push_back(BOXED_FLOAT);
    v_ff.push_back(BOXED_FLOAT);
    v_fi.push_back(BOXED_FLOAT);
    v_fi.push_back(BOXED_INT);
    v_uu.push_back(UNKNOWN);
    v_uu.push_back(UNKNOWN);

    FunctionMetadata* md = new FunctionMetadata(2, false, false);
    md->addVersion(float_func, rtn_type, v_ff);
    md->addVersion(int_func, rtn_type, v_fi);
    md->addVersion(boxed_func, UNKNOWN, v_uu);
    float_cls->giveAttr(name, new BoxedFunction(md));
}

static void _addFuncPow(const char* name, ConcreteCompilerType* rtn_type, void* float_func, void* int_func,
                        void* boxed_func) {
    std::vector<ConcreteCompilerType*> v_ffu{ BOXED_FLOAT, BOXED_FLOAT, UNKNOWN };
    std::vector<ConcreteCompilerType*> v_fiu{ BOXED_FLOAT, BOXED_INT, UNKNOWN };
    std::vector<ConcreteCompilerType*> v_fuu{ BOXED_FLOAT, UNKNOWN, UNKNOWN };

    FunctionMetadata* md = new FunctionMetadata(3, false, false);
    md->addVersion(float_func, rtn_type, v_ffu);
    md->addVersion(int_func, rtn_type, v_fiu);
    md->addVersion(boxed_func, UNKNOWN, v_fuu);
    float_cls->giveAttr(name, new BoxedFunction(md, { None }));
}

static Box* float_conjugate(Box* b, void*) noexcept {
    if (!PyFloat_Check(b)) {
        PyErr_Format(TypeError, "descriptor 'conjugate' requires a 'float' object but received a '%s'", getTypeName(b));
        return NULL;
    }
    if (b->cls == float_cls) {
        return incref(b);
    } else {
        assert(PyFloat_Check(b));
        return boxFloat(static_cast<BoxedFloat*>(b)->d);
    }
}

static Box* float0(Box*, void*) noexcept {
    return boxFloat(0.0);
}

PyDoc_STRVAR(float_getformat_doc, "float.__getformat__(typestr) -> string\n"
                                  "\n"
                                  "You probably don't want to use this function.  It exists mainly to be\n"
                                  "used in Python's test suite.\n"
                                  "\n"
                                  "typestr must be 'double' or 'float'.  This function returns whichever of\n"
                                  "'unknown', 'IEEE, big-endian' or 'IEEE, little-endian' best describes the\n"
                                  "format of floating point numbers used by the C type named by typestr.");

PyDoc_STRVAR(float_setformat_doc, "float.__setformat__(typestr, fmt) -> None\n"
                                  "\n"
                                  "You probably don't want to use this function.  It exists mainly to be\n"
                                  "used in Python's test suite.\n"
                                  "\n"
                                  "typestr must be 'double' or 'float'.  fmt must be one of 'unknown',\n"
                                  "'IEEE, big-endian' or 'IEEE, little-endian', and in addition can only be\n"
                                  "one of the latter two if it appears to match the underlying C reality.\n"
                                  "\n"
                                  "Override the automatic determination of C-level floating point type.\n"
                                  "This affects how floats are converted to and from binary strings.");

static PyMethodDef float_methods[]
    = { { "hex", (PyCFunction)float_hex, METH_NOARGS, NULL },
        { "fromhex", (PyCFunction)float_fromhex, METH_O | METH_CLASS, NULL },
        { "as_integer_ratio", (PyCFunction)float_as_integer_ratio, METH_NOARGS, NULL },
        { "__getformat__", (PyCFunction)float_getformat, METH_O | METH_CLASS, float_getformat_doc },
        { "__setformat__", (PyCFunction)float_setformat, METH_VARARGS | METH_CLASS, float_setformat_doc },
        { "is_integer", (PyCFunction)float_is_integer, METH_NOARGS, NULL },
        { "__format__", (PyCFunction)float__format__, METH_VARARGS, NULL },
        { NULL, NULL, 0, NULL } };

void setupFloat() {
    static PyNumberMethods float_as_number;
    float_cls->tp_as_number = &float_as_number;

    float_cls->giveAttr("__getnewargs__", new BoxedFunction(FunctionMetadata::create((void*)float_getnewargs, UNKNOWN,
                                                                                     1, ParamNames::empty(), CAPI)));

    _addFunc("__add__", BOXED_FLOAT, (void*)floatAddFloat, (void*)floatAddInt, (void*)floatAdd);
    float_cls->giveAttrBorrowed("__radd__", float_cls->getattr(autoDecref(internStringMortal("__add__"))));

    _addFunc("__div__", BOXED_FLOAT, (void*)floatDivFloat, (void*)floatDivInt, (void*)floatDiv);
    _addFunc("__rdiv__", BOXED_FLOAT, (void*)floatRDivFloat, (void*)floatRDivInt, (void*)floatRDiv);
    _addFunc("__floordiv__", BOXED_FLOAT, (void*)floatFloorDivFloat, (void*)floatFloorDivInt, (void*)floatFloorDiv);
    float_cls->giveAttr("__rfloordiv__",
                        new BoxedFunction(FunctionMetadata::create((void*)floatRFloorDiv, UNKNOWN, 2)));
    _addFunc("__truediv__", BOXED_FLOAT, (void*)floatDivFloat, (void*)floatDivInt, (void*)floatTruediv);
    float_cls->giveAttr("__rtruediv__", new BoxedFunction(FunctionMetadata::create((void*)floatRTruediv, UNKNOWN, 2)));

    _addFunc("__mod__", BOXED_FLOAT, (void*)floatModFloat, (void*)floatModInt, (void*)floatMod);
    _addFunc("__rmod__", BOXED_FLOAT, (void*)floatRModFloat, (void*)floatRModInt, (void*)floatRMod);
    _addFunc("__mul__", BOXED_FLOAT, (void*)floatMulFloat, (void*)floatMulInt, (void*)floatMul);
    float_cls->giveAttrBorrowed("__rmul__", float_cls->getattr(autoDecref(internStringMortal("__mul__"))));

    _addFuncPow("__pow__", BOXED_FLOAT, (void*)floatPowFloat, (void*)floatPowInt, (void*)floatPow);
    float_cls->giveAttr("__rpow__", new BoxedFunction(FunctionMetadata::create((void*)floatRPow, UNKNOWN, 2)));
    _addFunc("__sub__", BOXED_FLOAT, (void*)floatSubFloat, (void*)floatSubInt, (void*)floatSub);
    _addFunc("__rsub__", BOXED_FLOAT, (void*)floatRSubFloat, (void*)floatRSubInt, (void*)floatRSub);
    float_cls->giveAttr("__divmod__", new BoxedFunction(FunctionMetadata::create((void*)floatDivmod, UNKNOWN, 2)));
    float_cls->giveAttr("__rdivmod__", new BoxedFunction(FunctionMetadata::create((void*)floatRDivmod, UNKNOWN, 2)));

    auto float_new = FunctionMetadata::create((void*)floatNew<CXX>, UNKNOWN, 2, false, false,
                                              ParamNames({ "", "x" }, "", ""), CXX);
    float_new->addVersion((void*)floatNew<CAPI>, UNKNOWN, CAPI);
    float_cls->giveAttr("__new__", new BoxedFunction(float_new, { autoDecref(boxFloat(0.0)) }));

    float_cls->giveAttr("__eq__", new BoxedFunction(FunctionMetadata::create((void*)floatEq, UNKNOWN, 2)));
    float_cls->giveAttr("__ne__", new BoxedFunction(FunctionMetadata::create((void*)floatNe, UNKNOWN, 2)));
    float_cls->giveAttr("__le__", new BoxedFunction(FunctionMetadata::create((void*)floatLe, UNKNOWN, 2)));
    float_cls->giveAttr("__lt__", new BoxedFunction(FunctionMetadata::create((void*)floatLt, UNKNOWN, 2)));
    float_cls->giveAttr("__ge__", new BoxedFunction(FunctionMetadata::create((void*)floatGe, UNKNOWN, 2)));
    float_cls->giveAttr("__gt__", new BoxedFunction(FunctionMetadata::create((void*)floatGt, UNKNOWN, 2)));
    float_cls->giveAttr("__neg__", new BoxedFunction(FunctionMetadata::create((void*)floatNeg, BOXED_FLOAT, 1)));
    float_cls->giveAttr("__pos__", new BoxedFunction(FunctionMetadata::create((void*)floatPos, BOXED_FLOAT, 1)));
    float_cls->giveAttr("__abs__", new BoxedFunction(FunctionMetadata::create((void*)floatAbs, BOXED_FLOAT, 1)));

    FunctionMetadata* nonzero = FunctionMetadata::create((void*)floatNonzeroUnboxed, BOOL, 1);
    nonzero->addVersion((void*)floatNonzero, UNKNOWN);
    float_cls->giveAttr("__nonzero__", new BoxedFunction(nonzero));

    // float_cls->giveAttr("__nonzero__", new BoxedFunction(FunctionMetadata::create((void*)floatNonzero, NULL, 1)));
    float_cls->giveAttr("__float__", new BoxedFunction(FunctionMetadata::create((void*)floatFloat, BOXED_FLOAT, 1)));
    float_cls->giveAttr("__str__", new BoxedFunction(FunctionMetadata::create((void*)floatStr<CXX>, STR, 1)));
    float_cls->giveAttr("__repr__", new BoxedFunction(FunctionMetadata::create((void*)floatRepr<CXX>, STR, 1)));
    float_cls->giveAttr("__coerce__", new BoxedFunction(FunctionMetadata::create((void*)floatCoerce, UNKNOWN, 2)));

    float_cls->giveAttr("__trunc__", new BoxedFunction(FunctionMetadata::create((void*)floatTrunc, UNKNOWN, 1)));
    float_cls->giveAttr("__int__", new BoxedFunction(FunctionMetadata::create((void*)floatInt, UNKNOWN, 1)));
    float_cls->giveAttr("__long__", new BoxedFunction(FunctionMetadata::create((void*)floatLong, UNKNOWN, 1)));
    float_cls->giveAttr("__hash__", new BoxedFunction(FunctionMetadata::create((void*)floatHash, BOXED_INT, 1)));

    float_cls->giveAttrDescriptor("real", float_conjugate, NULL);
    float_cls->giveAttrDescriptor("imag", float0, NULL);
    float_cls->giveAttr("conjugate", new BoxedFunction(FunctionMetadata::create((void*)float_conjugate, BOXED_FLOAT, 1,
                                                                                ParamNames::empty(), CAPI)));

    float_cls->giveAttr("__doc__", boxString("float(x) -> floating point number\n"
                                             "\n"
                                             "Convert a string or number to a floating point number, if possible."));

    add_methods(float_cls, float_methods);

    add_operators(float_cls);
    float_cls->freeze();

    _PyFloat_Init();

    float_cls->tp_new = (newfunc)floatNewPacked;
    float_cls->tp_repr = (reprfunc)floatRepr<CAPI>;
    float_cls->tp_str = (reprfunc)floatStr<CAPI>;
    float_cls->tp_richcompare = float_richcompare;
    float_cls->tp_as_number->nb_int = (unaryfunc)float_to_int;
    float_cls->tp_as_number->nb_power = float_pow;
}
}
