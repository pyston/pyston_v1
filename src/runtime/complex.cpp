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

#include "runtime/complex.h"

#include "core/types.h"
#include "runtime/float.h"
#include "runtime/inline/boxing.h"
#include "runtime/objmodel.h"
#include "runtime/types.h"

namespace pyston {

static inline void raiseDivZeroExc() {
    raiseExcHelper(ZeroDivisionError, "complex divide by zero");
}

extern "C" Box* createPureImaginary(double i) {
    return new BoxedComplex(0.0, i);
}

static PyObject* try_complex_special_method(PyObject* op) noexcept {
    PyObject* f;
    static PyObject* complexstr;

    if (complexstr == NULL) {
        complexstr = PyString_InternFromString("__complex__");
        if (complexstr == NULL)
            return NULL;
    }
    if (PyInstance_Check(op)) {
        f = PyObject_GetAttr(op, complexstr);
        if (f == NULL) {
            if (PyErr_ExceptionMatches(PyExc_AttributeError))
                PyErr_Clear();
            else
                return NULL;
        }
    } else {
        f = _PyObject_LookupSpecial(op, "__complex__", &complexstr);
        if (f == NULL && PyErr_Occurred())
            return NULL;
    }
    if (f != NULL) {
        PyObject* res = PyObject_CallFunctionObjArgs(f, NULL);
        Py_DECREF(f);
        return res;
    }
    return NULL;
}

extern "C" Py_complex PyComplex_AsCComplex(PyObject* op) noexcept {
    Py_complex cv;
    PyObject* newop = NULL;

    assert(op);
    /* If op is already of type PyComplex_Type, return its value */
    if (PyComplex_Check(op)) {
        cv.real = ((BoxedComplex*)op)->real;
        cv.imag = ((BoxedComplex*)op)->imag;
        return cv;
    }
    /* If not, use op's __complex__  method, if it exists */

    /* return -1 on failure */
    cv.real = -1.;
    cv.imag = 0.;

    newop = try_complex_special_method(op);

    if (newop) {
        if (!PyComplex_Check(newop)) {
            PyErr_SetString(PyExc_TypeError, "__complex__ should return a complex object");
            Py_DECREF(newop);
            return cv;
        }
        cv.real = ((BoxedComplex*)newop)->real;
        cv.imag = ((BoxedComplex*)newop)->imag;
        Py_DECREF(newop);
        return cv;
    } else if (PyErr_Occurred()) {
        return cv;
    }
    /* If neither of the above works, interpret op as a float giving the
       real part of the result, and fill in the imaginary part as 0. */
    else {
        /* PyFloat_AsDouble will return -1 on failure */
        cv.real = PyFloat_AsDouble(op);
        return cv;
    }
}

extern "C" double PyComplex_RealAsDouble(PyObject* op) noexcept {
    if (PyComplex_Check(op)) {
        return static_cast<BoxedComplex*>(op)->real;
    } else {
        return PyFloat_AsDouble(op);
    }
}

extern "C" double PyComplex_ImagAsDouble(PyObject* op) noexcept {
    if (PyComplex_Check(op)) {
        return static_cast<BoxedComplex*>(op)->imag;
    } else {
        return 0.0;
    }
}

extern "C" PyObject* PyComplex_FromDoubles(double real, double imag) noexcept {
    return new BoxedComplex(real, imag);
}

extern "C" PyObject* PyComplex_FromCComplex(Py_complex val) noexcept {
    return new BoxedComplex(val.real, val.imag);
}


// addition

extern "C" Box* complexAddComplex(BoxedComplex* lhs, BoxedComplex* rhs) {
    assert(lhs->cls == complex_cls);
    assert(rhs->cls == complex_cls);
    return boxComplex(lhs->real + rhs->real, lhs->imag + rhs->imag);
}

extern "C" Box* complexAddFloat(BoxedComplex* lhs, BoxedFloat* rhs) {
    assert(lhs->cls == complex_cls);
    assert(rhs->cls == float_cls);
    return boxComplex(lhs->real + rhs->d, lhs->imag);
}

extern "C" Box* complexAddInt(BoxedComplex* lhs, BoxedInt* rhs) {
    assert(lhs->cls == complex_cls);
    assert(PyInt_Check(rhs));
    return boxComplex(lhs->real + (double)rhs->n, lhs->imag);
}

extern "C" Box* complexAdd(BoxedComplex* lhs, Box* rhs) {
    assert(lhs->cls == complex_cls);
    if (PyInt_Check(rhs)) {
        return complexAddInt(lhs, static_cast<BoxedInt*>(rhs));
    } else if (rhs->cls == float_cls) {
        return complexAddFloat(lhs, static_cast<BoxedFloat*>(rhs));
    } else if (rhs->cls == complex_cls) {
        return complexAddComplex(lhs, static_cast<BoxedComplex*>(rhs));
    } else {
        return NotImplemented;
    }
}

// subtraction

extern "C" Box* complexSubComplex(BoxedComplex* lhs, BoxedComplex* rhs) {
    assert(lhs->cls == complex_cls);
    assert(rhs->cls == complex_cls);
    return boxComplex(lhs->real - rhs->real, lhs->imag - rhs->imag);
}

extern "C" Box* complexSubFloat(BoxedComplex* lhs, BoxedFloat* rhs) {
    assert(lhs->cls == complex_cls);
    assert(rhs->cls == float_cls);
    return boxComplex(lhs->real - rhs->d, lhs->imag);
}

extern "C" Box* complexSubInt(BoxedComplex* lhs, BoxedInt* rhs) {
    assert(lhs->cls == complex_cls);
    assert(PyInt_Check(rhs));
    return boxComplex(lhs->real - (double)rhs->n, lhs->imag);
}

extern "C" Box* complexSub(BoxedComplex* lhs, Box* rhs) {
    assert(lhs->cls == complex_cls);
    if (PyInt_Check(rhs)) {
        return complexSubInt(lhs, static_cast<BoxedInt*>(rhs));
    } else if (rhs->cls == float_cls) {
        return complexSubFloat(lhs, static_cast<BoxedFloat*>(rhs));
    } else if (rhs->cls == complex_cls) {
        return complexSubComplex(lhs, static_cast<BoxedComplex*>(rhs));
    } else {
        return NotImplemented;
    }
}

extern "C" Box* complexRSub(BoxedComplex* _lhs, Box* _rhs) {
    if (!PyComplex_Check(_lhs))
        raiseExcHelper(TypeError, "descriptor '__rsub__' requires a 'complex' object but received a '%s'",
                       getTypeName(_lhs));
    BoxedComplex* lhs = new BoxedComplex(0.0, 0.0);
    if (PyInt_Check(_rhs)) {
        lhs->real = (static_cast<BoxedInt*>(_rhs))->n;
    } else if (_rhs->cls == float_cls) {
        lhs->real = (static_cast<BoxedFloat*>(_rhs))->d;
    } else if (_rhs->cls == complex_cls) {
        lhs = static_cast<BoxedComplex*>(_rhs);
    } else {
        return NotImplemented;
    }
    return complexSubComplex(lhs, _lhs);
}

// multiplication

extern "C" Box* complexMulComplex(BoxedComplex* lhs, BoxedComplex* rhs) {
    assert(lhs->cls == complex_cls);
    assert(rhs->cls == complex_cls);
    return boxComplex(lhs->real * rhs->real - lhs->imag * rhs->imag, lhs->real * rhs->imag + lhs->imag * rhs->real);
}

extern "C" Box* complexMulFloat(BoxedComplex* lhs, BoxedFloat* rhs) {
    assert(lhs->cls == complex_cls);
    assert(rhs->cls == float_cls);
    return boxComplex(lhs->real * rhs->d, lhs->imag * rhs->d);
}

extern "C" Box* complexMulInt(BoxedComplex* lhs, BoxedInt* rhs) {
    assert(lhs->cls == complex_cls);
    assert(PyInt_Check(rhs));
    return boxComplex(lhs->real * (double)rhs->n, lhs->imag * (double)rhs->n);
}

extern "C" Box* complexMul(BoxedComplex* lhs, Box* rhs) {
    assert(lhs->cls == complex_cls);
    if (PyInt_Check(rhs)) {
        return complexMulInt(lhs, static_cast<BoxedInt*>(rhs));
    } else if (rhs->cls == float_cls) {
        return complexMulFloat(lhs, static_cast<BoxedFloat*>(rhs));
    } else if (rhs->cls == complex_cls) {
        return complexMulComplex(lhs, static_cast<BoxedComplex*>(rhs));
    } else {
        return NotImplemented;
    }
}

// division
Box* complexDivComplex(BoxedComplex* lhs, BoxedComplex* rhs) {
    if (!PyComplex_Check(lhs))
        raiseExcHelper(TypeError, "descriptor '__div__' requires a 'complex' object but received a '%s'",
                       getTypeName(lhs));
    assert(rhs->cls == complex_cls);

    double real_f, imag_f;
    const double abs_breal = rhs->real < 0 ? -rhs->real : rhs->real;
    const double abs_bimag = rhs->imag < 0 ? -rhs->imag : rhs->imag;

    if (abs_breal >= abs_bimag) {
        /* divide tops and bottom by rhs.real */
        if (abs_breal == 0.0) {
            raiseDivZeroExc();
            real_f = imag_f = 0.0;
        } else {
            const double ratio = rhs->imag / rhs->real;
            const double denom = rhs->real + rhs->imag * ratio;
            real_f = (lhs->real + lhs->imag * ratio) / denom;
            imag_f = (lhs->imag - lhs->real * ratio) / denom;
        }
    } else {
        /* divide tops and bottom by rhs->imag */
        const double ratio = rhs->real / rhs->imag;
        const double denom = rhs->real * ratio + rhs->imag;
        real_f = (lhs->real * ratio + lhs->imag) / denom;
        imag_f = (lhs->imag * ratio - lhs->real) / denom;
    }
    return boxComplex(real_f, imag_f);
}

extern "C" Box* complexDivFloat(BoxedComplex* lhs, BoxedFloat* rhs) {
    if (!PyComplex_Check(lhs))
        raiseExcHelper(TypeError, "descriptor '__div__' requires a 'complex' object but received a '%s'",
                       getTypeName(lhs));
    assert(rhs->cls == float_cls);
    if (rhs->d == 0.0) {
        raiseDivZeroExc();
    }
    return boxComplex(lhs->real / rhs->d, lhs->imag / rhs->d);
}

extern "C" Box* complexDivInt(BoxedComplex* lhs, BoxedInt* rhs) {
    if (!PyComplex_Check(lhs))
        raiseExcHelper(TypeError, "descriptor '__div__' requires a 'complex' object but received a '%s'",
                       getTypeName(lhs));
    assert(PyInt_Check(rhs));
    if (rhs->n == 0) {
        raiseDivZeroExc();
    }
    return boxComplex(lhs->real / (double)rhs->n, lhs->imag / (double)rhs->n);
}

extern "C" Box* complexDiv(BoxedComplex* lhs, Box* rhs) {
    if (!PyComplex_Check(lhs))
        raiseExcHelper(TypeError, "descriptor '__div__' requires a 'complex' object but received a '%s'",
                       getTypeName(lhs));
    if (PyInt_Check(rhs)) {
        return complexDivInt(lhs, static_cast<BoxedInt*>(rhs));
    } else if (rhs->cls == float_cls) {
        return complexDivFloat(lhs, static_cast<BoxedFloat*>(rhs));
    } else if (rhs->cls == complex_cls) {
        return complexDivComplex(lhs, static_cast<BoxedComplex*>(rhs));
    } else {
        return NotImplemented;
    }
}

extern "C" Box* complexRDiv(BoxedComplex* _lhs, Box* _rhs) {
    if (!PyComplex_Check(_lhs))
        raiseExcHelper(TypeError, "descriptor '__rdiv__' requires a 'complex' object but received a '%s'",
                       getTypeName(_lhs));
    BoxedComplex* lhs = new BoxedComplex(0.0, 0.0);
    if (PyInt_Check(_rhs)) {
        lhs->real = (static_cast<BoxedInt*>(_rhs))->n;
    } else if (_rhs->cls == float_cls) {
        lhs->real = (static_cast<BoxedFloat*>(_rhs))->d;
    } else if (_rhs->cls == complex_cls) {
        lhs = static_cast<BoxedComplex*>(_rhs);
    } else {
        return NotImplemented;
    }
    return complexDivComplex(lhs, _lhs);
}

Box* complexPos(BoxedComplex* self) {
    if (!PyComplex_Check(self))
        raiseExcHelper(TypeError, "descriptor '__pos__' requires a 'complex' object but received a '%s'",
                       getTypeName(self));
    return PyComplex_FromDoubles(self->real, self->imag);
}

// str and repr
Box* complex_fmt(double r, double i, int precision, char format_code) noexcept {
    PyObject* result = NULL;

    /* If these are non-NULL, they'll need to be freed. */
    char* pre = NULL;
    char* im = NULL;

    /* These do not need to be freed. re is either an alias
       for pre or a pointer to a constant.  lead and tail
       are pointers to constants. */
    std::string lead = "";
    std::string tail = "";
    std::string re = "";
    std::string result_str;

    if (r == 0. && copysign(1.0, r) == 1.0) {
        re = "";
        im = PyOS_double_to_string(i, format_code, precision, 0, NULL);
        if (!im) {
            PyErr_NoMemory();
            goto done;
        }
    } else {
        /* Format imaginary part with signr part without */
        pre = PyOS_double_to_string(r, format_code, precision, 0, NULL);
        if (!pre) {
            PyErr_NoMemory();
            goto done;
        }
        re = std::string(pre);

        im = PyOS_double_to_string(i, format_code, precision, Py_DTSF_SIGN, NULL);
        if (!im) {
            PyErr_NoMemory();
            goto done;
        }
        lead = "(";
        tail = ")";
    }
    /* Alloc the final buffer. Add one for the "j" in the format string,
       and one for the trailing zero. */
    result_str = lead + std::string(re) + std::string(im) + "j" + tail;
    result = PyString_FromString(result_str.c_str());
done:
    PyMem_Free(im);
    PyMem_Free(pre);

    return result;
}


static void _addFunc(const char* name, ConcreteCompilerType* rtn_type, void* complex_func, void* float_func,
                     void* int_func, void* boxed_func) {
    CLFunction* cl = createRTFunction(2, false, false);
    addRTFunction(cl, complex_func, rtn_type, { BOXED_COMPLEX, BOXED_COMPLEX });
    addRTFunction(cl, float_func, rtn_type, { BOXED_COMPLEX, BOXED_FLOAT });
    addRTFunction(cl, int_func, rtn_type, { BOXED_COMPLEX, BOXED_INT });
    addRTFunction(cl, boxed_func, UNKNOWN, { BOXED_COMPLEX, UNKNOWN });
    complex_cls->giveAttr(name, new BoxedFunction(cl));
}

static Py_complex c_1 = { 1., 0. };

extern "C" Py_complex c_prod(Py_complex a, Py_complex b) noexcept {
    Py_complex r;
    r.real = a.real * b.real - a.imag * b.imag;
    r.imag = a.real * b.imag + a.imag * b.real;
    return r;
}

extern "C" Py_complex c_quot(Py_complex a, Py_complex b) noexcept {
    /******************************************************************
    This was the original algorithm.  It's grossly prone to spurious
    overflow and underflow errors.  It also merrily divides by 0 despite
    checking for that(!).  The code still serves a doc purpose here, as
    the algorithm following is a simple by-cases transformation of this
    one:

    Py_complex r;
    double d = b.real*b.real + b.imag*b.imag;
    if (d == 0.)
        errno = EDOM;
    r.real = (a.real*b.real + a.imag*b.imag)/d;
    r.imag = (a.imag*b.real - a.real*b.imag)/d;
    return r;
    ******************************************************************/

    /* This algorithm is better, and is pretty obvious:  first divide the
     * numerators and denominator by whichever of {b.real, b.imag} has
     * larger magnitude.  The earliest reference I found was to CACM
     * Algorithm 116 (Complex Division, Robert L. Smith, Stanford
     * University).  As usual, though, we're still ignoring all IEEE
     * endcases.
     */
    Py_complex r; /* the result */
    const double abs_breal = b.real < 0 ? -b.real : b.real;
    const double abs_bimag = b.imag < 0 ? -b.imag : b.imag;

    if (abs_breal >= abs_bimag) {
        /* divide tops and bottom by b.real */
        if (abs_breal == 0.0) {
            errno = EDOM;
            r.real = r.imag = 0.0;
        } else {
            const double ratio = b.imag / b.real;
            const double denom = b.real + b.imag * ratio;
            r.real = (a.real + a.imag * ratio) / denom;
            r.imag = (a.imag - a.real * ratio) / denom;
        }
    } else {
        /* divide tops and bottom by b.imag */
        const double ratio = b.real / b.imag;
        const double denom = b.real * ratio + b.imag;
        assert(b.imag != 0.0);
        r.real = (a.real * ratio + a.imag) / denom;
        r.imag = (a.imag * ratio - a.real) / denom;
    }
    return r;
}

extern "C" Py_complex c_pow(Py_complex a, Py_complex b) noexcept {
    Py_complex r;
    double vabs, len, at, phase;
    if (b.real == 0. && b.imag == 0.) {
        r.real = 1.;
        r.imag = 0.;
    } else if (a.real == 0. && a.imag == 0.) {
        if (b.imag != 0. || b.real < 0.)
            errno = EDOM;
        r.real = 0.;
        r.imag = 0.;
    } else {
        vabs = hypot(a.real, a.imag);
        len = pow(vabs, b.real);
        at = atan2(a.imag, a.real);
        phase = at * b.real;
        if (b.imag != 0.0) {
            len /= exp(at * b.imag);
            phase += b.imag * log(vabs);
        }
        r.real = len * cos(phase);
        r.imag = len * sin(phase);
    }
    return r;
}

static Py_complex c_powu(Py_complex x, long n) noexcept {
    Py_complex r, p;
    long mask = 1;
    r = c_1;
    p = x;
    while (mask > 0 && n >= mask) {
        if (n & mask)
            r = c_prod(r, p);
        mask <<= 1;
        p = c_prod(p, p);
    }
    return r;
}

static Py_complex c_powi(Py_complex x, long n) noexcept {
    Py_complex cn;

    if (n > 100 || n < -100) {
        cn.real = (double)n;
        cn.imag = 0.;
        return c_pow(x, cn);
    } else if (n > 0)
        return c_powu(x, n);
    else
        return c_quot(c_1, c_powu(x, -n));
}

Box* complexPow(BoxedComplex* lhs, Box* _rhs, Box* mod) {
    if (!PyComplex_Check(lhs))
        raiseExcHelper(TypeError, "descriptor '__pow__' requires a 'complex' object but received a '%s'",
                       getTypeName(lhs));
    Py_complex p;
    Py_complex exponent;
    long int_exponent;
    Py_complex a, b;
    a = PyComplex_AsCComplex(lhs);
    b = PyComplex_AsCComplex(_rhs);
    if (mod != Py_None) {
        raiseExcHelper(ValueError, "complex modulo");
    }
    PyFPE_START_PROTECT("complex_pow", return 0) errno = 0;
    exponent = b;
    int_exponent = (long)exponent.real;
    if (exponent.imag == 0. && exponent.real == int_exponent)
        p = c_powi(a, int_exponent);
    else
        p = c_pow(a, exponent);

    PyFPE_END_PROTECT(p) Py_ADJUST_ERANGE2(p.real, p.imag);
    if (errno == EDOM) {
        raiseExcHelper(ZeroDivisionError, "0.0 to a negative or complex power");
    } else if (errno == ERANGE) {
        raiseExcHelper(OverflowError, "complex exponentiation");
    }

    return boxComplex(p.real, p.imag);
}

Box* complexHash(BoxedComplex* self) {
    if (!PyComplex_Check(self))
        raiseExcHelper(TypeError, "descriptor '__hash__' requires a 'complex' object but received a '%s'",
                       getTypeName(self));
    long hashreal, hashimag, combined;

    hashreal = _Py_HashDouble(self->real);
    if (hashreal == -1) {
        throwCAPIException();
    }

    hashimag = _Py_HashDouble(self->imag);
    if (hashimag == -1) {
        throwCAPIException();
    }
    /* Note:  if the imaginary part is 0, hashimag is 0 now,
     * so the following returns hashreal unchanged.  This is
     * important because numbers of different types that
     * compare equal must have the same hash value, so that
     * hash(x + 0*j) must equal hash(x).
     */
    combined = hashreal + 1000003 * hashimag;
    if (combined == -1)
        combined = -2;
    return boxInt(combined);
}

Box* complexCoerce(Box* lhs, Box* rhs) {
    Py_complex cval;
    cval.imag = 0.;
    if (PyInt_Check(rhs)) {
        cval.real = (double)PyInt_AsLong(rhs);
        rhs = PyComplex_FromCComplex(cval);
    } else if (PyLong_Check(rhs)) {
        cval.real = PyLong_AsDouble(rhs);
        if (cval.real == -1.0 && PyErr_Occurred()) {
            throwCAPIException();
        }
        rhs = PyComplex_FromCComplex(cval);
    } else if (PyFloat_Check(rhs)) {
        cval.real = PyFloat_AsDouble(rhs);
        rhs = PyComplex_FromCComplex(cval);
    } else if (!PyComplex_Check(rhs)) {
        return NotImplemented;
    }
    return BoxedTuple::create({ lhs, rhs });
}

Box* complexConjugate(BoxedComplex* self) {
    if (!PyComplex_Check(self))
        raiseExcHelper(TypeError, "descriptor 'conjuagte' requires a 'complex' object but received a '%s'",
                       getTypeName(self));
    return new BoxedComplex(self->real, -self->imag);
}

Box* complexAbs(BoxedComplex* self) {
    if (!PyComplex_Check(self))
        raiseExcHelper(TypeError, "descriptor '__abs__' requires a 'complex' object but received a '%s'",
                       getTypeName(self));
    double result;

    if (isinf(self->real) || isinf(self->imag)) {
        /* C99 rules: if either the real or the imaginary part is an
           infinity, return infinity, even if the other part is a
           NaN. */
        if (!isinf(self->real)) {
            return boxFloat(fabs(self->real));
        }
        if (!isinf(self->imag)) {
            return boxFloat(fabs(self->imag));
        }
        /* either the real or imaginary part is a NaN,
           and neither is infinite. Result should be NaN. */
        return boxFloat(Py_NAN);
    }

    result = sqrt(self->real * self->real + self->imag * self->imag);

    if (isinf(result)) {
        raiseExcHelper(OverflowError, "absolute value too large");
    }

    return boxFloat(result);
}

Box* complexGetnewargs(BoxedComplex* self) {
    if (!PyComplex_Check(self))
        raiseExcHelper(TypeError, "descriptor '__getnewargs__' requires a 'complex' object but received a '%s'",
                       getTypeName(self));
    return BoxedTuple::create({ boxFloat(self->real), boxFloat(self->imag) });
}

Box* complexNonzero(BoxedComplex* self) {
    if (!PyComplex_Check(self))
        raiseExcHelper(TypeError, "descriptor '__nonzero__' require a 'complex' object but received a '%s'",
                       getTypeName(self));
    bool res = self->real != 0.0 || self->imag != 0.0;
    return boxBool(res);
}

Box* complexStr(BoxedComplex* self) {
    if (!PyComplex_Check(self))
        raiseExcHelper(TypeError, "descriptor '__str__' requires a 'complex' object but received a '%s'",
                       getTypeName(self));
    Box* r = complex_fmt(self->real, self->imag, 12, 'g');
    if (!r) {
        throwCAPIException();
    }
    return r;
}

Box* complexRepr(BoxedComplex* self) {
    if (!PyComplex_Check(self))
        raiseExcHelper(TypeError, "descriptor '__repr__' requires a 'complex' object but received a '%s'",
                       getTypeName(self));
    Box* r = complex_fmt(self->real, self->imag, 16, 'g');
    if (!r) {
        throwCAPIException();
    }
    return r;
}

static PyObject* complex_subtype_from_string(PyObject* v) noexcept {
    const char* s, *start;
    char* end;
    double x = 0.0, y = 0.0, z;
    int got_bracket = 0;
#ifdef Py_USING_UNICODE
    char* s_buffer = NULL;
#endif
    Py_ssize_t len;

    if (PyString_Check(v)) {
        s = PyString_AS_STRING(v);
        len = PyString_GET_SIZE(v);
    }
#ifdef Py_USING_UNICODE
    else if (PyUnicode_Check(v)) {
        s_buffer = (char*)PyMem_MALLOC(PyUnicode_GET_SIZE(v) + 1);
        if (s_buffer == NULL)
            return PyErr_NoMemory();
        if (PyUnicode_EncodeDecimal(PyUnicode_AS_UNICODE(v), PyUnicode_GET_SIZE(v), s_buffer, NULL))
            goto error;
        s = s_buffer;
        len = strlen(s);
    }
#endif
    else if (PyObject_AsCharBuffer(v, &s, &len)) {
        PyErr_SetString(PyExc_TypeError, "complex() arg is not a string");
        return NULL;
    }

    /* position on first nonblank */
    start = s;
    while (Py_ISSPACE(*s))
        s++;
    if (*s == '(') {
        /* Skip over possible bracket from repr(). */
        got_bracket = 1;
        s++;
        while (Py_ISSPACE(*s))
            s++;
    }

    /* a valid complex string usually takes one of the three forms:

         <float>                  - real part only
         <float>j                 - imaginary part only
         <float><signed-float>j   - real and imaginary parts

       where <float> represents any numeric string that's accepted by the
       float constructor (including 'nan', 'inf', 'infinity', etc.), and
       <signed-float> is any string of the form <float> whose first
       character is '+' or '-'.

       For backwards compatibility, the extra forms

         <float><sign>j
         <sign>j
         j

       are also accepted, though support for these forms may be removed from
       a future version of Python.
    */

    /* first look for forms starting with <float> */
    z = PyOS_string_to_double(s, &end, NULL);
    if (z == -1.0 && PyErr_Occurred()) {
        if (PyErr_ExceptionMatches(PyExc_ValueError))
            PyErr_Clear();
        else
            goto error;
    }
    if (end != s) {
        /* all 4 forms starting with <float> land here */
        s = end;
        if (*s == '+' || *s == '-') {
            /* <float><signed-float>j | <float><sign>j */
            x = z;
            y = PyOS_string_to_double(s, &end, NULL);
            if (y == -1.0 && PyErr_Occurred()) {
                if (PyErr_ExceptionMatches(PyExc_ValueError))
                    PyErr_Clear();
                else
                    goto error;
            }
            if (end != s)
                /* <float><signed-float>j */
                s = end;
            else {
                /* <float><sign>j */
                y = *s == '+' ? 1.0 : -1.0;
                s++;
            }
            if (!(*s == 'j' || *s == 'J'))
                goto parse_error;
            s++;
        } else if (*s == 'j' || *s == 'J') {
            /* <float>j */
            s++;
            y = z;
        } else
            /* <float> */
            x = z;
    } else {
        /* not starting with <float>; must be <sign>j or j */
        if (*s == '+' || *s == '-') {
            /* <sign>j */
            y = *s == '+' ? 1.0 : -1.0;
            s++;
        } else
            /* j */
            y = 1.0;
        if (!(*s == 'j' || *s == 'J'))
            goto parse_error;
        s++;
    }

    /* trailing whitespace and closing bracket */
    while (Py_ISSPACE(*s))
        s++;
    if (got_bracket) {
        /* if there was an opening parenthesis, then the corresponding
           closing parenthesis should be right here */
        if (*s != ')')
            goto parse_error;
        s++;
        while (Py_ISSPACE(*s))
            s++;
    }

    /* we should now be at the end of the string */
    if (s - start != len)
        goto parse_error;


#ifdef Py_USING_UNICODE
    if (s_buffer)
        PyMem_FREE(s_buffer);
#endif
    return new BoxedComplex(x, y);

parse_error:
    PyErr_SetString(PyExc_ValueError, "complex() arg is a malformed string");
error:
#ifdef Py_USING_UNICODE
    if (s_buffer)
        PyMem_FREE(s_buffer);
#endif
    return NULL;
}


static Box* to_complex(Box* self) noexcept {
    BoxedComplex* r;
    if (self == None || self == NULL) {
        return new BoxedComplex(0.0, 0.0);
    }
    static BoxedString* complex_str = internStringImmortal("__complex__");

    if (PyComplex_Check(self) && !PyObject_HasAttr((PyObject*)self, complex_str)) {
        r = (BoxedComplex*)self;
    } else if (PyInt_Check(self)) {
        r = new BoxedComplex(static_cast<BoxedInt*>(self)->n, 0.0);
    } else if (PyFloat_Check(self)) {
        r = new BoxedComplex((static_cast<BoxedFloat*>(PyNumber_Float(self)))->d, 0.0);
    } else if (self->cls == long_cls) {
        r = new BoxedComplex(PyLong_AsDouble(self), 0.0);
    } else {
        return NotImplemented;
    }
    return r;
}

template <ExceptionStyle S> static Box* try_special_method(Box* self) noexcept(S == CAPI) {
    if (self == None || self == NULL) {
        return None;
    }

    static BoxedString* float_str = internStringImmortal("__float__");
    if (PyObject_HasAttr((PyObject*)self, float_str)) {
        Box* r_f = callattrInternal<S>(self, float_str, CLASS_ONLY, NULL, ArgPassSpec(0), NULL, NULL, NULL, NULL, NULL);
        if (!PyFloat_Check(r_f)) {
            if (S == CAPI) {
                if (!PyErr_Occurred())
                    PyErr_Format(PyExc_TypeError, "__float__ returned non-float (type %.200s)", r_f->cls->tp_name);
                return NULL;
            } else {
                raiseExcHelper(TypeError, "__float__ returned non-float (type %.200s)", r_f->cls->tp_name);
            }
        }
        return (BoxedFloat*)r_f;
    }

    static BoxedString* complex_str = internStringImmortal("__complex__");
    if (PyObject_HasAttr((PyObject*)self, complex_str)) {
        Box* r
            = callattrInternal<S>(self, complex_str, CLASS_OR_INST, NULL, ArgPassSpec(0), NULL, NULL, NULL, NULL, NULL);
        if (!r) {
            if (S == CAPI) {
                if (!PyErr_Occurred())
                    PyErr_Format(TypeError, "complex() argument must be a string or a number, not '%s'\n",
                                 getTypeName(self));
                return NULL;
            } else {
                raiseExcHelper(TypeError, "complex() argument must be a string or a number, not '%s'\n",
                               getTypeName(self));
            }
        }

        if (!PyComplex_Check(r)) {
            if (S == CAPI) {
                PyErr_Format(TypeError, "__complex__ returned non-complex (type %s)", r->cls->tp_name);
                return NULL;
            } else
                raiseExcHelper(TypeError, "__complex__ returned non-complex (type %s)", r->cls->tp_name);
        }

        return static_cast<BoxedComplex*>(r);
    }
    return None;
}

template <ExceptionStyle S> Box* _complexNew(Box* real, Box* imag) noexcept(S == CAPI) {
    // handle str and unicode
    if (real != None && real != NULL) {
        if (PyString_Check(real) || PyUnicode_Check(real)) {
            if (imag != None && imag != NULL) {
                raiseExcHelper(TypeError, "complex() can't take second arg if first is a string");
            }
            Box* res = complex_subtype_from_string(real);
            if (res == NULL) {
                throwCAPIException();
            }
            return res;
        }
    }

    if (real != None && real != NULL && real->cls == complex_cls && (imag == NULL || imag == None)) {
        return static_cast<BoxedComplex*>(real);
    }

    Box* _real = try_special_method<S>(real);
    Box* _imag = try_special_method<S>(imag);

    if (_real != None && _real != NULL) {
        real = _real;
    }

    if (_imag != None && _imag != NULL) {
        imag = _imag;
    }

    double real_f, imag_f;
    bool real_is_complex = false, imag_is_complex = false;
    if (real == None || real == NULL) {
        real_f = 0.0;
    } else if (PyComplex_Check(real)) {
        real_f = ((BoxedComplex*)real)->real;
        real_is_complex = true;
    } else {
        real_f = ((BoxedFloat*)PyNumber_Float(real))->d;
    }
    if (imag == None || imag == NULL) {
        imag_f = 0.0;
    } else if (PyComplex_Check(imag)) {
        imag_f = ((BoxedComplex*)imag)->real;
        imag_is_complex = true;
    } else {
        imag_f = ((BoxedFloat*)PyNumber_Float(imag))->d;
    }

    if (imag_is_complex) {
        real_f -= ((BoxedComplex*)imag)->imag;
    }

    if (real_is_complex) {
        imag_f += ((BoxedComplex*)real)->imag;
    }

    return new BoxedComplex(real_f, imag_f);
}

template <ExceptionStyle S> Box* complexNew(BoxedClass* _cls, Box* _real, Box* _imag) noexcept(S == CAPI) {
    if (!isSubclass(_cls->cls, type_cls)) {
        if (S == CAPI) {
            PyErr_Format(TypeError, "complex.__new__(X): X is not a type object (%s)", getTypeName(_cls));
            return NULL;
        } else
            raiseExcHelper(TypeError, "complex.__new__(X): X is not a type object (%s)", getTypeName(_cls));
    }

    BoxedClass* cls = static_cast<BoxedClass*>(_cls);
    if (PyComplex_Check(cls)) {
        if (S == CAPI) {
            PyErr_Format(TypeError, "complex.__new__(%s): %s is not a subtype of complex", getNameOfClass(cls),
                         getNameOfClass(cls));
            return NULL;
        } else
            raiseExcHelper(TypeError, "complex.__new__(%s): %s is not a subtype of complex", getNameOfClass(cls),
                           getNameOfClass(cls));
    }

    // check second argument
    if (_imag != NULL && (PyString_Check(_imag) || PyUnicode_Check(_imag))) {
        if (S == CAPI) {
            PyErr_Format(TypeError, "complex() second arg can't be a string");
            return NULL;
        } else
            raiseExcHelper(TypeError, "complex() second arg can't be a string");
    }

    // we can't use None as default value, because complex(None) should raise TypeError,
    // but complex() should return `0j`. So use NULL as default value.
    // Here check we pass complex(None, None), complex(None)
    // or complex(imag=None) to the constructor
    if ((_real == None) && (_imag == None || _imag == NULL)) {
        raiseExcHelper(TypeError, "complex() argument must be a string or number");
    }

    if (cls == complex_cls)
        return _complexNew<S>(_real, _imag);

    BoxedComplex* r = (BoxedComplex*)_complexNew<S>(_real, _imag);

    if (!r) {
        assert(S == CAPI);
        return NULL;
    }

    return new (_cls) BoxedComplex(r->real, r->imag);
}

extern "C" Box* complexDivmod(BoxedComplex* lhs, BoxedComplex* rhs) {
    if (!PyComplex_Check(lhs))
        raiseExcHelper(TypeError, "descriptor '__divmod__' requires a 'complex' object but received a '%s'",
                       getTypeName(lhs));
    if (PyErr_Warn(PyExc_DeprecationWarning, "complex divmod(), // and % are deprecated") < 0)
        return NULL;

    if (rhs->real == 0.0 && rhs->imag == 0.0) {
        raiseExcHelper(ZeroDivisionError, "complex divmod()");
    }

    BoxedComplex* div = (BoxedComplex*)complexDiv(lhs, rhs); /* The raw divisor value. */
    div->real = floor(div->real);                            /* Use the floor of the real part. */
    div->imag = 0.0;
    BoxedComplex* mod = (BoxedComplex*)complexSubComplex(lhs, (BoxedComplex*)complexMulComplex(rhs, div));
    Box* res = BoxedTuple::create({ div, mod });
    return res;
}

extern "C" Box* complexMod(BoxedComplex* lhs, Box* _rhs) {
    if (!PyComplex_Check(lhs))
        raiseExcHelper(TypeError, "descriptor '__mod__' requires a 'complex' object but received a '%s'",
                       getTypeName(lhs));
    if (PyErr_Warn(PyExc_DeprecationWarning, "complex divmod(), // and % are deprecated") < 0)
        return NULL;

    Box* res = to_complex(_rhs);
    if (res == NotImplemented) {
        return NotImplemented;
    }

    BoxedComplex* rhs = (BoxedComplex*)res;

    if (rhs->real == 0.0 && rhs->imag == 0.0) {
        raiseExcHelper(ZeroDivisionError, "complex remainder");
    }

    BoxedComplex* div = (BoxedComplex*)complexDiv(lhs, rhs); /* The raw divisor value. */
    div->real = floor(div->real);                            /* Use the floor of the real part. */
    div->imag = 0.0;
    BoxedComplex* mod = (BoxedComplex*)complexSubComplex(lhs, (BoxedComplex*)complexMulComplex(rhs, div));
    return mod;
}

extern "C" Box* complexFloordiv(BoxedComplex* lhs, Box* _rhs) {
    if (!PyComplex_Check(lhs))
        raiseExcHelper(TypeError, "descriptor '__floordiv__' requires a 'complex' object but received a '%s'",
                       getTypeName(lhs));

    Box* res = to_complex(_rhs);
    if (res == NotImplemented) {
        return NotImplemented;
    }
    BoxedComplex* rhs = (BoxedComplex*)res;

    BoxedTuple* t = (BoxedTuple*)complexDivmod(lhs, rhs);
    return t->elts[0];
}

static PyObject* complex_richcompare(PyObject* v, PyObject* w, int op) noexcept {
    PyObject* res;
    int equal;

    if (op != Py_EQ && op != Py_NE) {
        /* for backwards compatibility, comparisons with non-numbers return
         * NotImplemented.  Only comparisons with core numeric types raise
         * TypeError.
         */
        if (PyInt_Check(w) || PyLong_Check(w) || PyFloat_Check(w) || PyComplex_Check(w)) {
            PyErr_SetString(PyExc_TypeError, "no ordering relation is defined "
                                             "for complex numbers");
            return NULL;
        }
        return Py_NotImplemented;
    }

    assert(PyComplex_Check(v));
    BoxedComplex* lhs = static_cast<BoxedComplex*>(v);

    if (PyInt_Check(w) || PyLong_Check(w)) {
        /* Check for 0->g0 imaginary part first to avoid the rich
         * comparison when possible->g
         */
        if (lhs->imag == 0.0) {
            PyObject* j, *sub_res;
            j = PyFloat_FromDouble(lhs->real);
            if (j == NULL)
                return NULL;

            sub_res = PyObject_RichCompare(j, w, op);
            Py_DECREF(j);
            return sub_res;
        } else {
            equal = 0;
        }
    } else if (PyFloat_Check(w)) {
        equal = (lhs->real == PyFloat_AsDouble(w) && lhs->imag == 0.0);
    } else if (PyComplex_Check(w)) {
        BoxedComplex* rhs = static_cast<BoxedComplex*>(w);
        equal = (lhs->real == rhs->real && lhs->imag == rhs->imag);
    } else {
        return Py_NotImplemented;
    }

    if (equal == (op == Py_EQ))
        res = Py_True;
    else
        res = Py_False;

    Py_INCREF(res);
    return res;
}

extern "C" Box* complexEq(BoxedComplex* lhs, Box* rhs) {
    if (lhs->cls != complex_cls) {
        raiseExcHelper(TypeError, "descriptor '__eq__' requires a 'complex' object but recieved a '%s'",
                       getTypeName(lhs));
    }
    Box* res = complex_richcompare(lhs, rhs, Py_EQ);

    if (!res) {
        throwCAPIException();
    }
    return res;
}

extern "C" Box* complexNe(BoxedComplex* lhs, Box* rhs) {
    if (lhs->cls != complex_cls) {
        raiseExcHelper(TypeError, "descriptor '__ne__' requires a 'complex' object but recieved a '%s'",
                       getTypeName(lhs));
    }
    Box* res = complex_richcompare(lhs, rhs, Py_NE);

    if (!res) {
        throwCAPIException();
    }
    return res;
}

extern "C" Box* complexLe(BoxedComplex* lhs, Box* rhs) {
    if (lhs->cls != complex_cls) {
        raiseExcHelper(TypeError, "descriptor '__le__' requires a 'complex' object but recieved a '%s'",
                       getTypeName(lhs));
    }
    Box* res = complex_richcompare(lhs, rhs, Py_LE);

    if (!res) {
        throwCAPIException();
    }
    return res;
}

extern "C" Box* complexLt(BoxedComplex* lhs, Box* rhs) {
    if (lhs->cls != complex_cls) {
        raiseExcHelper(TypeError, "descriptor '__lt__' requires a 'complex' object but recieved a '%s'",
                       getTypeName(lhs));
    }
    Box* res = complex_richcompare(lhs, rhs, Py_LT);
    if (!res) {
        throwCAPIException();
    }
    return res;
}

extern "C" Box* complexGe(BoxedComplex* lhs, Box* rhs) {
    if (lhs->cls != complex_cls) {
        raiseExcHelper(TypeError, "descriptor '__ge__' requires a 'complex' object but recieved a '%s'",
                       getTypeName(lhs));
    }
    Box* res = complex_richcompare(lhs, rhs, Py_GE);

    if (!res) {
        throwCAPIException();
    }
    return res;
}

extern "C" Box* complexGt(BoxedComplex* lhs, Box* rhs) {
    if (lhs->cls != complex_cls) {
        raiseExcHelper(TypeError, "descriptor '__gt__' requires a 'complex' object but recieved a '%s'",
                       getTypeName(lhs));
    }
    Box* res = complex_richcompare(lhs, rhs, Py_GT);

    if (!res) {
        throwCAPIException();
    }
    return res;
}

Box* complexNeg(Box* _self) {
    assert(_self->cls == complex_cls);
    BoxedComplex* self = (BoxedComplex*)_self;
    return PyComplex_FromDoubles(-self->real, -self->imag);
}

PyObject* complex_neg(PyComplexObject* v) {
    BoxedComplex* self = (BoxedComplex*)v;
    return PyComplex_FromDoubles(-self->real, -self->imag);
}

// Pyston change: make not static
PyObject* complex__format__(PyObject* self, PyObject* args) {
    PyObject* format_spec;

    if (!PyArg_ParseTuple(args, "O:__format__", &format_spec))
        return NULL;
    if (PyBytes_Check(format_spec))
        return _PyComplex_FormatAdvanced(self, PyBytes_AS_STRING(format_spec), PyBytes_GET_SIZE(format_spec));
    if (PyUnicode_Check(format_spec)) {
        /* Convert format_spec to a str */
        PyObject* result;
        PyObject* str_spec = PyObject_Str(format_spec);

        if (str_spec == NULL)
            return NULL;

        result = _PyComplex_FormatAdvanced(self, PyBytes_AS_STRING(str_spec), PyBytes_GET_SIZE(str_spec));

        Py_DECREF(str_spec);
        return result;
    }
    PyErr_SetString(PyExc_TypeError, "__format__ requires str or unicode");
    return NULL;
}

static PyMethodDef complex_methods[] = {
    { "__format__", (PyCFunction)complex__format__, METH_VARARGS, NULL },
};

void setupComplex() {
    auto complex_new = boxRTFunction((void*)complexNew<CXX>, UNKNOWN, 3, false, false,
                                     ParamNames({ "", "real", "imag" }, "", ""), CXX);
    addRTFunction(complex_new, (void*)complexNew<CAPI>, UNKNOWN, CAPI);
    complex_cls->giveAttr("__new__", new BoxedFunction(complex_new, { NULL, NULL }));

    _addFunc("__add__", BOXED_COMPLEX, (void*)complexAddComplex, (void*)complexAddFloat, (void*)complexAddInt,
             (void*)complexAdd);
    _addFunc("__radd__", BOXED_COMPLEX, (void*)complexAddComplex, (void*)complexAddFloat, (void*)complexAddInt,
             (void*)complexAdd);

    _addFunc("__sub__", BOXED_COMPLEX, (void*)complexSubComplex, (void*)complexSubFloat, (void*)complexSubInt,
             (void*)complexSub);

    _addFunc("__mul__", BOXED_COMPLEX, (void*)complexMulComplex, (void*)complexMulFloat, (void*)complexMulInt,
             (void*)complexMul);
    _addFunc("__rmul__", BOXED_COMPLEX, (void*)complexMulComplex, (void*)complexMulFloat, (void*)complexMulInt,
             (void*)complexMul);

    _addFunc("__div__", BOXED_COMPLEX, (void*)complexDivComplex, (void*)complexDivFloat, (void*)complexDivInt,
             (void*)complexDiv);

    complex_cls->giveAttr("__rsub__", new BoxedFunction(boxRTFunction((void*)complexRSub, UNKNOWN, 2)));
    complex_cls->giveAttr("__rdiv__", new BoxedFunction(boxRTFunction((void*)complexRDiv, UNKNOWN, 2)));
    complex_cls->giveAttr("__pow__",
                          new BoxedFunction(boxRTFunction((void*)complexPow, UNKNOWN, 3, false, false), { None }));
    complex_cls->giveAttr("__mod__", new BoxedFunction(boxRTFunction((void*)complexMod, UNKNOWN, 2)));
    complex_cls->giveAttr("__divmod__", new BoxedFunction(boxRTFunction((void*)complexDivmod, BOXED_TUPLE, 2)));
    complex_cls->giveAttr("__floordiv__", new BoxedFunction(boxRTFunction((void*)complexFloordiv, UNKNOWN, 2)));

    complex_cls->giveAttr("__truediv__", new BoxedFunction(boxRTFunction((void*)complexDivComplex, BOXED_COMPLEX, 2)));
    complex_cls->giveAttr("conjugate", new BoxedFunction(boxRTFunction((void*)complexConjugate, BOXED_COMPLEX, 1)));
    complex_cls->giveAttr("__coerce__", new BoxedFunction(boxRTFunction((void*)complexCoerce, UNKNOWN, 2)));
    complex_cls->giveAttr("__abs__", new BoxedFunction(boxRTFunction((void*)complexAbs, BOXED_FLOAT, 1)));
    complex_cls->giveAttr("__getnewargs__", new BoxedFunction(boxRTFunction((void*)complexGetnewargs, BOXED_TUPLE, 1)));
    complex_cls->giveAttr("__nonzero__", new BoxedFunction(boxRTFunction((void*)complexNonzero, BOXED_BOOL, 1)));
    complex_cls->giveAttr("__eq__", new BoxedFunction(boxRTFunction((void*)complexEq, UNKNOWN, 2)));
    complex_cls->giveAttr("__ne__", new BoxedFunction(boxRTFunction((void*)complexNe, UNKNOWN, 2)));
    complex_cls->giveAttr("__le__", new BoxedFunction(boxRTFunction((void*)complexLe, UNKNOWN, 2)));
    complex_cls->giveAttr("__lt__", new BoxedFunction(boxRTFunction((void*)complexLt, UNKNOWN, 2)));
    complex_cls->giveAttr("__ge__", new BoxedFunction(boxRTFunction((void*)complexGe, UNKNOWN, 2)));
    complex_cls->giveAttr("__gt__", new BoxedFunction(boxRTFunction((void*)complexGt, UNKNOWN, 2)));
    complex_cls->giveAttr("__neg__", new BoxedFunction(boxRTFunction((void*)complexNeg, BOXED_COMPLEX, 1)));
    complex_cls->giveAttr("__pos__", new BoxedFunction(boxRTFunction((void*)complexPos, BOXED_COMPLEX, 1)));
    complex_cls->giveAttr("__hash__", new BoxedFunction(boxRTFunction((void*)complexHash, BOXED_INT, 1)));
    complex_cls->giveAttr("__str__", new BoxedFunction(boxRTFunction((void*)complexStr, STR, 1)));
    complex_cls->giveAttr("__repr__", new BoxedFunction(boxRTFunction((void*)complexRepr, STR, 1)));
    complex_cls->giveAttr("real",
                          new BoxedMemberDescriptor(BoxedMemberDescriptor::DOUBLE, offsetof(BoxedComplex, real)));
    complex_cls->giveAttr("imag",
                          new BoxedMemberDescriptor(BoxedMemberDescriptor::DOUBLE, offsetof(BoxedComplex, imag)));
    for (auto& md : complex_methods) {
        complex_cls->giveAttr(md.ml_name, new BoxedMethodDescriptor(&md, complex_cls));
    }

    complex_cls->freeze();
    complex_cls->tp_as_number->nb_negative = (unaryfunc)complex_neg;
    complex_cls->tp_richcompare = complex_richcompare;
}

void teardownComplex() {
}
}
