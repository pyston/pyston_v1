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

// This file is mostly copied from CPython

#include "Python.h"

#include "core/types.h"
#include "runtime/types.h"

namespace pyston {

extern "C" {

typedef enum { unknown_format, ieee_big_endian_format, ieee_little_endian_format } float_format_type;

static float_format_type double_format, float_format;
static float_format_type detected_double_format, detected_float_format;

static PyObject* float_getformat(PyTypeObject* v, PyObject* arg) noexcept {
    float_format_type r;

    BoxedString* str = static_cast<BoxedString*>(arg);

    if (!PyString_Check(arg)) {
        PyErr_Format(PyExc_TypeError, "__getformat__() argument must be string, not %.500s", Py_TYPE(arg)->tp_name);
        return NULL;
    }
    if (str->s == "double") {
        r = double_format;
    } else if (str->s == "float") {
        r = float_format;
    } else {
        PyErr_SetString(PyExc_ValueError, "__getformat__() argument 1 must be "
                                          "'double' or 'float'");
        return NULL;
    }

    switch (r) {
        case unknown_format:
            return static_cast<PyObject*>(boxStrConstant("unknown"));
        case ieee_little_endian_format:
            return static_cast<PyObject*>(boxStrConstant("IEEE, little-endian"));
        case ieee_big_endian_format:
            return static_cast<PyObject*>(boxStrConstant("IEEE, big-endian"));
        default:
            Py_FatalError("insane float_format or double_format");
            return NULL;
    }
}

PyDoc_STRVAR(float_getformat_doc, "float.__getformat__(typestr) -> string\n"
                                  "\n"
                                  "You probably don't want to use this function.  It exists mainly to be\n"
                                  "used in Python's test suite.\n"
                                  "\n"
                                  "typestr must be 'double' or 'float'.  This function returns whichever of\n"
                                  "'unknown', 'IEEE, big-endian' or 'IEEE, little-endian' best describes the\n"
                                  "format of floating point numbers used by the C type named by typestr.");

static PyObject* float_setformat(PyTypeObject* v, PyObject* args) noexcept {
    char* typestr;
    char* format;
    float_format_type f;
    float_format_type detected;
    float_format_type* p;

    if (!PyArg_ParseTuple(args, "ss:__setformat__", &typestr, &format))
        return NULL;

    if (strcmp(typestr, "double") == 0) {
        p = &double_format;
        detected = detected_double_format;
    } else if (strcmp(typestr, "float") == 0) {
        p = &float_format;
        detected = detected_float_format;
    } else {
        PyErr_SetString(PyExc_ValueError, "__setformat__() argument 1 must "
                                          "be 'double' or 'float'");
        return NULL;
    }

    if (strcmp(format, "unknown") == 0) {
        f = unknown_format;
    } else if (strcmp(format, "IEEE, little-endian") == 0) {
        f = ieee_little_endian_format;
    } else if (strcmp(format, "IEEE, big-endian") == 0) {
        f = ieee_big_endian_format;
    } else {
        PyErr_SetString(PyExc_ValueError, "__setformat__() argument 2 must be "
                                          "'unknown', 'IEEE, little-endian' or "
                                          "'IEEE, big-endian'");
        return NULL;
    }

    if (f != unknown_format && f != detected) {
        PyErr_Format(PyExc_ValueError, "can only set %s format to 'unknown' or the "
                                       "detected platform value",
                     typestr);
        return NULL;
    }

    *p = f;
    Py_RETURN_NONE;
}

/*----------------------------------------------------------------------------
 * _PyFloat_{Pack,Unpack}{4,8}.  See floatobject.h.
 */
extern "C" int _PyFloat_Pack4(double x, unsigned char* p, int le) noexcept {
    if (float_format == unknown_format) {
        unsigned char sign;
        int e;
        double f;
        unsigned int fbits;
        int incr = 1;

        if (le) {
            p += 3;
            incr = -1;
        }

        if (x < 0) {
            sign = 1;
            x = -x;
        } else
            sign = 0;

        f = frexp(x, &e);

        /* Normalize f to be in the range [1.0, 2.0) */
        if (0.5 <= f && f < 1.0) {
            f *= 2.0;
            e--;
        } else if (f == 0.0)
            e = 0;
        else {
            PyErr_SetString(PyExc_SystemError, "frexp() result out of range");
            return -1;
        }

        if (e >= 128)
            goto Overflow;
        else if (e < -126) {
            /* Gradual underflow */
            f = ldexp(f, 126 + e);
            e = 0;
        } else if (!(e == 0 && f == 0.0)) {
            e += 127;
            f -= 1.0; /* Get rid of leading 1 */
        }

        f *= 8388608.0;                  /* 2**23 */
        fbits = (unsigned int)(f + 0.5); /* Round */
        assert(fbits <= 8388608);
        if (fbits >> 23) {
            /* The carry propagated out of a string of 23 1 bits. */
            fbits = 0;
            ++e;
            if (e >= 255)
                goto Overflow;
        }

        /* First byte */
        *p = (sign << 7) | (e >> 1);
        p += incr;

        /* Second byte */
        *p = (char)(((e & 1) << 7) | (fbits >> 16));
        p += incr;

        /* Third byte */
        *p = (fbits >> 8) & 0xFF;
        p += incr;

        /* Fourth byte */
        *p = fbits & 0xFF;

        /* Done */
        return 0;

    } else {
        float y = (float)x;
        const char* s = (char*)&y;
        int i, incr = 1;

        if (Py_IS_INFINITY(y) && !Py_IS_INFINITY(x))
            goto Overflow;

        if ((float_format == ieee_little_endian_format && !le) || (float_format == ieee_big_endian_format && le)) {
            p += 3;
            incr = -1;
        }

        for (i = 0; i < 4; i++) {
            *p = *s++;
            p += incr;
        }
        return 0;
    }
Overflow:
    PyErr_SetString(PyExc_OverflowError, "float too large to pack with f format");
    return -1;
}

extern "C" int _PyFloat_Pack8(double x, unsigned char* p, int le) noexcept {
    if (double_format == unknown_format) {
        unsigned char sign;
        int e;
        double f;
        unsigned int fhi, flo;
        int incr = 1;

        if (le) {
            p += 7;
            incr = -1;
        }

        if (x < 0) {
            sign = 1;
            x = -x;
        } else
            sign = 0;

        f = frexp(x, &e);

        /* Normalize f to be in the range [1.0, 2.0) */
        if (0.5 <= f && f < 1.0) {
            f *= 2.0;
            e--;
        } else if (f == 0.0)
            e = 0;
        else {
            PyErr_SetString(PyExc_SystemError, "frexp() result out of range");
            return -1;
        }

        if (e >= 1024)
            goto Overflow;
        else if (e < -1022) {
            /* Gradual underflow */
            f = ldexp(f, 1022 + e);
            e = 0;
        } else if (!(e == 0 && f == 0.0)) {
            e += 1023;
            f -= 1.0; /* Get rid of leading 1 */
        }

        /* fhi receives the high 28 bits; flo the low 24 bits (== 52 bits) */
        f *= 268435456.0;      /* 2**28 */
        fhi = (unsigned int)f; /* Truncate */
        assert(fhi < 268435456);

        f -= (double)fhi;
        f *= 16777216.0;               /* 2**24 */
        flo = (unsigned int)(f + 0.5); /* Round */
        assert(flo <= 16777216);
        if (flo >> 24) {
            /* The carry propagated out of a string of 24 1 bits. */
            flo = 0;
            ++fhi;
            if (fhi >> 28) {
                /* And it also progagated out of the next 28 bits. */
                fhi = 0;
                ++e;
                if (e >= 2047)
                    goto Overflow;
            }
        }

        /* First byte */
        *p = (sign << 7) | (e >> 4);
        p += incr;

        /* Second byte */
        *p = (unsigned char)(((e & 0xF) << 4) | (fhi >> 24));
        p += incr;

        /* Third byte */
        *p = (fhi >> 16) & 0xFF;
        p += incr;

        /* Fourth byte */
        *p = (fhi >> 8) & 0xFF;
        p += incr;

        /* Fifth byte */
        *p = fhi & 0xFF;
        p += incr;

        /* Sixth byte */
        *p = (flo >> 16) & 0xFF;
        p += incr;

        /* Seventh byte */
        *p = (flo >> 8) & 0xFF;
        p += incr;

        /* Eighth byte */
        *p = flo & 0xFF;
        /* p += incr; Unneeded (for now) */

        /* Done */
        return 0;

    Overflow:
        PyErr_SetString(PyExc_OverflowError, "float too large to pack with d format");
        return -1;
    } else {
        const char* s = (char*)&x;
        int i, incr = 1;

        if ((double_format == ieee_little_endian_format && !le) || (double_format == ieee_big_endian_format && le)) {
            p += 7;
            incr = -1;
        }

        for (i = 0; i < 8; i++) {
            *p = *s++;
            p += incr;
        }
        return 0;
    }
}

double _PyFloat_Unpack4(const unsigned char* p, int le) noexcept {
    if (float_format == unknown_format) {
        unsigned char sign;
        int e;
        unsigned int f;
        double x;
        int incr = 1;

        if (le) {
            p += 3;
            incr = -1;
        }

        /* First byte */
        sign = (*p >> 7) & 1;
        e = (*p & 0x7F) << 1;
        p += incr;

        /* Second byte */
        e |= (*p >> 7) & 1;
        f = (*p & 0x7F) << 16;
        p += incr;

        if (e == 255) {
            PyErr_SetString(PyExc_ValueError, "can't unpack IEEE 754 special value "
                                              "on non-IEEE platform");
            return -1;
        }

        /* Third byte */
        f |= *p << 8;
        p += incr;

        /* Fourth byte */
        f |= *p;

        x = (double)f / 8388608.0;

        /* XXX This sadly ignores Inf/NaN issues */
        if (e == 0)
            e = -126;
        else {
            x += 1.0;
            e -= 127;
        }
        x = ldexp(x, e);

        if (sign)
            x = -x;

        return x;
    } else {
        float x;

        if ((float_format == ieee_little_endian_format && !le) || (float_format == ieee_big_endian_format && le)) {
            char buf[4];
            char* d = &buf[3];
            int i;

            for (i = 0; i < 4; i++) {
                *d-- = *p++;
            }
            memcpy(&x, buf, 4);
        } else {
            memcpy(&x, p, 4);
        }

        return x;
    }
}

double _PyFloat_Unpack8(const unsigned char* p, int le) noexcept {
    if (double_format == unknown_format) {
        unsigned char sign;
        int e;
        unsigned int fhi, flo;
        double x;
        int incr = 1;

        if (le) {
            p += 7;
            incr = -1;
        }

        /* First byte */
        sign = (*p >> 7) & 1;
        e = (*p & 0x7F) << 4;

        p += incr;

        /* Second byte */
        e |= (*p >> 4) & 0xF;
        fhi = (*p & 0xF) << 24;
        p += incr;

        if (e == 2047) {
            PyErr_SetString(PyExc_ValueError, "can't unpack IEEE 754 special value "
                                              "on non-IEEE platform");
            return -1.0;
        }

        /* Third byte */
        fhi |= *p << 16;
        p += incr;

        /* Fourth byte */
        fhi |= *p << 8;
        p += incr;

        /* Fifth byte */
        fhi |= *p;
        p += incr;

        /* Sixth byte */
        flo = *p << 16;
        p += incr;

        /* Seventh byte */
        flo |= *p << 8;
        p += incr;

        /* Eighth byte */
        flo |= *p;

        x = (double)fhi + (double)flo / 16777216.0; /* 2**24 */
        x /= 268435456.0;                           /* 2**28 */

        if (e == 0)
            e = -1022;
        else {
            x += 1.0;
            e -= 1023;
        }
        x = ldexp(x, e);

        if (sign)
            x = -x;

        return x;
    } else {
        double x;

        if ((double_format == ieee_little_endian_format && !le) || (double_format == ieee_big_endian_format && le)) {
            char buf[8];
            char* d = &buf[7];
            int i;

            for (i = 0; i < 8; i++) {
                *d-- = *p++;
            }
            memcpy(&x, buf, 8);
        } else {
            memcpy(&x, p, 8);
        }

        return x;
    }
}
}
}
