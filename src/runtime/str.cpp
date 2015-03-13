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

#include <algorithm>
#include <cstring>
#include <sstream>
#include <unordered_map>

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/raw_ostream.h"

#include "Python.h"

#include "capi/types.h"
#include "core/common.h"
#include "core/types.h"
#include "core/util.h"
#include "gc/collector.h"
#include "runtime/capi.h"
#include "runtime/dict.h"
#include "runtime/long.h"
#include "runtime/objmodel.h"
#include "runtime/types.h"
#include "runtime/util.h"

extern "C" PyObject* string_split(PyStringObject* self, PyObject* args) noexcept;
extern "C" PyObject* string_rsplit(PyStringObject* self, PyObject* args) noexcept;
extern "C" PyObject* string_find(PyStringObject* self, PyObject* args) noexcept;
extern "C" PyObject* string_rfind(PyStringObject* self, PyObject* args) noexcept;

namespace pyston {

BoxedString::BoxedString(const char* s, size_t n) : s(s, n) {
    gc::registerGCManagedBytes(this->s.size());
}

BoxedString::BoxedString(std::string&& s) : s(std::move(s)) {
    gc::registerGCManagedBytes(this->s.size());
}

BoxedString::BoxedString(const std::string& s) : s(s) {
    gc::registerGCManagedBytes(this->s.size());
}

extern "C" char PyString_GetItem(PyObject* op, ssize_t n) noexcept {
    RELEASE_ASSERT(PyString_Check(op), "");
    return static_cast<const BoxedString*>(op)->s[n];
}

extern "C" PyObject* PyString_FromFormatV(const char* format, va_list vargs) noexcept {
    va_list count;
    Py_ssize_t n = 0;
    const char* f;
    char* s;
    PyObject* string;

#ifdef VA_LIST_IS_ARRAY
    Py_MEMCPY(count, vargs, sizeof(va_list));
#else
#ifdef __va_copy
    __va_copy(count, vargs);
#else
    count = vargs;
#endif
#endif
    /* step 1: figure out how large a buffer we need */
    for (f = format; *f; f++) {
        if (*f == '%') {
#ifdef HAVE_LONG_LONG
            int longlongflag = 0;
#endif
            const char* p = f;
            while (*++f && *f != '%' && !isalpha(Py_CHARMASK(*f)))
                ;

            /* skip the 'l' or 'z' in {%ld, %zd, %lu, %zu} since
             * they don't affect the amount of space we reserve.
             */
            if (*f == 'l') {
                if (f[1] == 'd' || f[1] == 'u') {
                    ++f;
                }
#ifdef HAVE_LONG_LONG
                else if (f[1] == 'l' && (f[2] == 'd' || f[2] == 'u')) {
                    longlongflag = 1;
                    f += 2;
                }
#endif
            } else if (*f == 'z' && (f[1] == 'd' || f[1] == 'u')) {
                ++f;
            }

            switch (*f) {
                case 'c':
                    (void)va_arg(count, int);
                /* fall through... */
                case '%':
                    n++;
                    break;
                case 'd':
                case 'u':
                case 'i':
                case 'x':
                    (void)va_arg(count, int);
#ifdef HAVE_LONG_LONG
                    /* Need at most
                       ceil(log10(256)*SIZEOF_LONG_LONG) digits,
                       plus 1 for the sign.  53/22 is an upper
                       bound for log10(256). */
                    if (longlongflag)
                        n += 2 + (SIZEOF_LONG_LONG * 53 - 1) / 22;
                    else
#endif
                        /* 20 bytes is enough to hold a 64-bit
                           integer.  Decimal takes the most
                           space.  This isn't enough for
                           octal. */
                        n += 20;

                    break;
                case 's':
                    s = va_arg(count, char*);
                    n += strlen(s);
                    break;
                case 'p':
                    (void)va_arg(count, int);
                    /* maximum 64-bit pointer representation:
                     * 0xffffffffffffffff
                     * so 19 characters is enough.
                     * XXX I count 18 -- what's the extra for?
                     */
                    n += 19;
                    break;
                default:
                    /* if we stumble upon an unknown
                       formatting code, copy the rest of
                       the format string to the output
                       string. (we cannot just skip the
                       code, since there's no way to know
                       what's in the argument list) */
                    n += strlen(p);
                    goto expand;
            }
        } else
            n++;
    }
expand:
    /* step 2: fill the buffer */
    /* Since we've analyzed how much space we need for the worst case,
       use sprintf directly instead of the slower PyOS_snprintf. */
    string = PyString_FromStringAndSize(NULL, n);
    if (!string)
        return NULL;

    s = PyString_AsString(string);

    for (f = format; *f; f++) {
        if (*f == '%') {
            const char* p = f++;
            Py_ssize_t i;
            int longflag = 0;
#ifdef HAVE_LONG_LONG
            int longlongflag = 0;
#endif
            int size_tflag = 0;
            /* parse the width.precision part (we're only
               interested in the precision value, if any) */
            n = 0;
            while (isdigit(Py_CHARMASK(*f)))
                n = (n * 10) + *f++ - '0';
            if (*f == '.') {
                f++;
                n = 0;
                while (isdigit(Py_CHARMASK(*f)))
                    n = (n * 10) + *f++ - '0';
            }
            while (*f && *f != '%' && !isalpha(Py_CHARMASK(*f)))
                f++;
            /* Handle %ld, %lu, %lld and %llu. */
            if (*f == 'l') {
                if (f[1] == 'd' || f[1] == 'u') {
                    longflag = 1;
                    ++f;
                }
#ifdef HAVE_LONG_LONG
                else if (f[1] == 'l' && (f[2] == 'd' || f[2] == 'u')) {
                    longlongflag = 1;
                    f += 2;
                }
#endif
            }
            /* handle the size_t flag. */
            else if (*f == 'z' && (f[1] == 'd' || f[1] == 'u')) {
                size_tflag = 1;
                ++f;
            }

            switch (*f) {
                case 'c':
                    *s++ = va_arg(vargs, int);
                    break;
                case 'd':
                    if (longflag)
                        sprintf(s, "%ld", va_arg(vargs, long));
#ifdef HAVE_LONG_LONG
                    else if (longlongflag)
                        sprintf(s, "%" PY_FORMAT_LONG_LONG "d", va_arg(vargs, PY_LONG_LONG));
#endif
                    else if (size_tflag)
                        sprintf(s, "%" PY_FORMAT_SIZE_T "d", va_arg(vargs, Py_ssize_t));
                    else
                        sprintf(s, "%d", va_arg(vargs, int));
                    s += strlen(s);
                    break;
                case 'u':
                    if (longflag)
                        sprintf(s, "%lu", va_arg(vargs, unsigned long));
#ifdef HAVE_LONG_LONG
                    else if (longlongflag)
                        sprintf(s, "%" PY_FORMAT_LONG_LONG "u", va_arg(vargs, PY_LONG_LONG));
#endif
                    else if (size_tflag)
                        sprintf(s, "%" PY_FORMAT_SIZE_T "u", va_arg(vargs, size_t));
                    else
                        sprintf(s, "%u", va_arg(vargs, unsigned int));
                    s += strlen(s);
                    break;
                case 'i':
                    sprintf(s, "%i", va_arg(vargs, int));
                    s += strlen(s);
                    break;
                case 'x':
                    sprintf(s, "%x", va_arg(vargs, int));
                    s += strlen(s);
                    break;
                case 's':
                    p = va_arg(vargs, char*);
                    i = strlen(p);
                    if (n > 0 && i > n)
                        i = n;
                    Py_MEMCPY(s, p, i);
                    s += i;
                    break;
                case 'p':
                    sprintf(s, "%p", va_arg(vargs, void*));
                    /* %p is ill-defined:  ensure leading 0x. */
                    if (s[1] == 'X')
                        s[1] = 'x';
                    else if (s[1] != 'x') {
                        memmove(s + 2, s, strlen(s) + 1);
                        s[0] = '0';
                        s[1] = 'x';
                    }
                    s += strlen(s);
                    break;
                case '%':
                    *s++ = '%';
                    break;
                default:
                    strcpy(s, p);
                    s += strlen(s);
                    goto end;
            }
        } else
            *s++ = *f;
    }

end:
    if (_PyString_Resize(&string, s - PyString_AS_STRING(string)))
        return NULL;
    return string;
}

extern "C" PyObject* PyString_FromFormat(const char* format, ...) noexcept {
    PyObject* ret;
    va_list vargs;

#ifdef HAVE_STDARG_PROTOTYPES
    va_start(vargs, format);
#else
    va_start(vargs);
#endif
    ret = PyString_FromFormatV(format, vargs);
    va_end(vargs);
    return ret;
}

extern "C" Box* strAdd(BoxedString* lhs, Box* _rhs) {
    assert(isSubclass(lhs->cls, str_cls));

    if (_rhs->cls == unicode_cls) {
        Box* rtn = PyUnicode_Concat(lhs, _rhs);
        checkAndThrowCAPIException();
        return rtn;
    }

    if (_rhs->cls != str_cls) {
        raiseExcHelper(TypeError, "cannot concatenate 'str' and '%s' objects", getTypeName(_rhs));
    }

    BoxedString* rhs = static_cast<BoxedString*>(_rhs);
    return new BoxedString(lhs->s + rhs->s);
}

extern "C" PyObject* PyString_InternFromString(const char* s) noexcept {
    return new BoxedString(s);
}

extern "C" void PyString_InternInPlace(PyObject**) noexcept {
    Py_FatalError("unimplemented");
}

/* Format codes
 * F_LJUST      '-'
 * F_SIGN       '+'
 * F_BLANK      ' '
 * F_ALT        '#'
 * F_ZERO       '0'
 */
#define F_LJUST (1 << 0)
#define F_SIGN (1 << 1)
#define F_BLANK (1 << 2)
#define F_ALT (1 << 3)
#define F_ZERO (1 << 4)

Py_LOCAL_INLINE(PyObject*) getnextarg(PyObject* args, Py_ssize_t arglen, Py_ssize_t* p_argidx) {
    Py_ssize_t argidx = *p_argidx;
    if (argidx < arglen) {
        (*p_argidx)++;
        if (arglen < 0)
            return args;
        else
            return PyTuple_GetItem(args, argidx);
    }
    PyErr_SetString(PyExc_TypeError, "not enough arguments for format string");
    return NULL;
}

extern "C" PyObject* _PyString_FormatLong(PyObject* val, int flags, int prec, int type, const char** pbuf,
                                          int* plen) noexcept {
    // Pyston change:
    RELEASE_ASSERT(val->cls == long_cls, "");


    PyObject* result = NULL;
    char* buf;
    Py_ssize_t i;
    int sign; /* 1 if '-', else 0 */
    int len;  /* number of characters */
    Py_ssize_t llen;
    int numdigits; /* len == numnondigits + numdigits */
    int numnondigits = 0;

    switch (type) {
        case 'd':
        case 'u':
            // Pyston change:
            // result = Py_TYPE(val)->tp_str(val);
            result = longStr((BoxedLong*)val);
            break;
        case 'o':
            // Pyston change:
            // result = Py_TYPE(val)->tp_as_number->nb_oct(val);
            result = longOct((BoxedLong*)val);
            break;
        case 'x':
        case 'X':
            numnondigits = 2;
            // Pyston change:
            // result = Py_TYPE(val)->tp_as_number->nb_hex(val);
            result = longHex((BoxedLong*)val);
            break;
        default:
            assert(!"'type' not in [duoxX]");
    }
    if (!result)
        return NULL;

    buf = PyString_AsString(result);
    if (!buf) {
        Py_DECREF(result);
        return NULL;
    }

    /* To modify the string in-place, there can only be one reference. */
    // Pyston change:
    // if (Py_REFCNT(result) != 1) {
    if (0) {
        PyErr_BadInternalCall();
        return NULL;
    }
    llen = PyString_Size(result);
    if (llen > INT_MAX) {
        PyErr_SetString(PyExc_ValueError, "string too large in _PyString_FormatLong");
        return NULL;
    }
    len = (int)llen;
    if (buf[len - 1] == 'L') {
        --len;
        buf[len] = '\0';
    }
    sign = buf[0] == '-';
    numnondigits += sign;
    numdigits = len - numnondigits;
    assert(numdigits > 0);

    /* Get rid of base marker unless F_ALT */
    if ((flags & F_ALT) == 0) {
        /* Need to skip 0x, 0X or 0. */
        int skipped = 0;
        switch (type) {
            case 'o':
                assert(buf[sign] == '0');
                /* If 0 is only digit, leave it alone. */
                if (numdigits > 1) {
                    skipped = 1;
                    --numdigits;
                }
                break;
            case 'x':
            case 'X':
                assert(buf[sign] == '0');
                assert(buf[sign + 1] == 'x');
                skipped = 2;
                numnondigits -= 2;
                break;
        }
        if (skipped) {
            buf += skipped;
            len -= skipped;
            if (sign)
                buf[0] = '-';
        }
        assert(len == numnondigits + numdigits);
        assert(numdigits > 0);
    }

    /* Fill with leading zeroes to meet minimum width. */
    if (prec > numdigits) {
        PyObject* r1 = PyString_FromStringAndSize(NULL, numnondigits + prec);
        char* b1;
        if (!r1) {
            Py_DECREF(result);
            return NULL;
        }
        b1 = PyString_AS_STRING(r1);
        for (i = 0; i < numnondigits; ++i)
            *b1++ = *buf++;
        for (i = 0; i < prec - numdigits; i++)
            *b1++ = '0';
        for (i = 0; i < numdigits; i++)
            *b1++ = *buf++;
        *b1 = '\0';
        Py_DECREF(result);
        result = r1;
        buf = PyString_AS_STRING(result);
        len = numnondigits + prec;
    }

    /* Fix up case for hex conversions. */
    if (type == 'X') {
        /* Need to convert all lower case letters to upper case.
           and need to convert 0x to 0X (and -0x to -0X). */
        for (i = 0; i < len; i++)
            if (buf[i] >= 'a' && buf[i] <= 'x')
                buf[i] -= 'a' - 'A';
    }
    *pbuf = buf;
    *plen = len;
    return result;
}

static PyObject* formatfloat(PyObject* v, int flags, int prec, int type) {
    char* p;
    PyObject* result;
    double x;

    x = PyFloat_AsDouble(v);
    if (x == -1.0 && PyErr_Occurred()) {
        PyErr_Format(PyExc_TypeError, "float argument required, "
                                      "not %.200s",
                     Py_TYPE(v)->tp_name);
        return NULL;
    }

    if (prec < 0)
        prec = 6;

    p = PyOS_double_to_string(x, type, prec, (flags & F_ALT) ? Py_DTSF_ALT : 0, NULL);

    if (p == NULL)
        return NULL;
    result = PyString_FromStringAndSize(p, strlen(p));
    PyMem_Free(p);
    return result;
}

Py_LOCAL_INLINE(int) formatint(char* buf, size_t buflen, int flags, int prec, int type, PyObject* v) {
    /* fmt = '%#.' + `prec` + 'l' + `type`
       worst case length = 3 + 19 (worst len of INT_MAX on 64-bit machine)
       + 1 + 1 = 24 */
    char fmt[64]; /* plenty big enough! */
    const char* sign;
    long x;

    x = PyInt_AsLong(v);
    if (x == -1 && PyErr_Occurred()) {
        PyErr_Format(PyExc_TypeError, "int argument required, not %.200s", Py_TYPE(v)->tp_name);
        return -1;
    }
    if (x < 0 && type == 'u') {
        type = 'd';
    }
    if (x < 0 && (type == 'x' || type == 'X' || type == 'o'))
        sign = "-";
    else
        sign = "";
    if (prec < 0)
        prec = 1;

    if ((flags & F_ALT) && (type == 'x' || type == 'X')) {
        /* When converting under %#x or %#X, there are a number
         * of issues that cause pain:
         * - when 0 is being converted, the C standard leaves off
         *   the '0x' or '0X', which is inconsistent with other
         *   %#x/%#X conversions and inconsistent with Python's
         *   hex() function
         * - there are platforms that violate the standard and
         *   convert 0 with the '0x' or '0X'
         *   (Metrowerks, Compaq Tru64)
         * - there are platforms that give '0x' when converting
         *   under %#X, but convert 0 in accordance with the
         *   standard (OS/2 EMX)
         *
         * We can achieve the desired consistency by inserting our
         * own '0x' or '0X' prefix, and substituting %x/%X in place
         * of %#x/%#X.
         *
         * Note that this is the same approach as used in
         * formatint() in unicodeobject.c
         */
        PyOS_snprintf(fmt, sizeof(fmt), "%s0%c%%.%dl%c", sign, type, prec, type);
    } else {
        PyOS_snprintf(fmt, sizeof(fmt), "%s%%%s.%dl%c", sign, (flags & F_ALT) ? "#" : "", prec, type);
    }

    /* buf = '+'/'-'/'' + '0'/'0x'/'' + '[0-9]'*max(prec, len(x in octal))
     * worst case buf = '-0x' + [0-9]*prec, where prec >= 11
     */
    if (buflen <= 14 || buflen <= (size_t)3 + (size_t)prec) {
        PyErr_SetString(PyExc_OverflowError, "formatted integer is too long (precision too large?)");
        return -1;
    }
    if (sign[0])
        PyOS_snprintf(buf, buflen, fmt, -x);
    else
        PyOS_snprintf(buf, buflen, fmt, x);
    return (int)strlen(buf);
}

Py_LOCAL_INLINE(int) formatchar(char* buf, size_t buflen, PyObject* v) {
    /* presume that the buffer is at least 2 characters long */
    if (PyString_Check(v)) {
        if (!PyArg_Parse(v, "c;%c requires int or char", &buf[0]))
            return -1;
    } else {
        if (!PyArg_Parse(v, "b;%c requires int or char", &buf[0]))
            return -1;
    }
    buf[1] = '\0';
    return 1;
}

#define FORMATBUFLEN (size_t)120
extern "C" PyObject* PyString_Format(PyObject* format, PyObject* args) noexcept {
    char* fmt, *res;
    Py_ssize_t arglen, argidx;
    Py_ssize_t reslen, rescnt, fmtcnt;
    int args_owned = 0;
    PyObject* result, *orig_args;
#ifdef Py_USING_UNICODE
    PyObject* v, *w;
#endif
    PyObject* dict = NULL;
    if (format == NULL || !PyString_Check(format) || args == NULL) {
        PyErr_BadInternalCall();
        return NULL;
    }
    orig_args = args;
    fmt = PyString_AS_STRING(format);
    fmtcnt = PyString_GET_SIZE(format);
    reslen = rescnt = fmtcnt + 100;
    result = PyString_FromStringAndSize((char*)NULL, reslen);
    if (result == NULL)
        return NULL;
    res = PyString_AsString(result);
    if (PyTuple_Check(args)) {
        arglen = PyTuple_GET_SIZE(args);
        argidx = 0;
    } else {
        arglen = -1;
        argidx = -2;
    }
    if (Py_TYPE(args)->tp_as_mapping && Py_TYPE(args)->tp_as_mapping->mp_subscript && !PyTuple_Check(args)
        && !PyObject_TypeCheck(args, &PyBaseString_Type))
        dict = args;
    while (--fmtcnt >= 0) {
        if (*fmt != '%') {
            if (--rescnt < 0) {
                rescnt = fmtcnt + 100;
                reslen += rescnt;
                if (_PyString_Resize(&result, reslen))
                    return NULL;
                res = PyString_AS_STRING(result) + reslen - rescnt;
                --rescnt;
            }
            *res++ = *fmt++;
        } else {
            /* Got a format specifier */
            int flags = 0;
            Py_ssize_t width = -1;
            int prec = -1;
            int c = '\0';
            int fill;
            int isnumok;
            PyObject* v = NULL;
            PyObject* temp = NULL;
            const char* pbuf;
            int sign;
            Py_ssize_t len;
            char formatbuf[FORMATBUFLEN];
/* For format{int,char}() */
#ifdef Py_USING_UNICODE
            char* fmt_start = fmt;
            Py_ssize_t argidx_start = argidx;
#endif

            fmt++;
            if (*fmt == '(') {
                char* keystart;
                Py_ssize_t keylen;
                PyObject* key;
                int pcount = 1;

                if (dict == NULL) {
                    PyErr_SetString(PyExc_TypeError, "format requires a mapping");
                    goto error;
                }
                ++fmt;
                --fmtcnt;
                keystart = fmt;
                /* Skip over balanced parentheses */
                while (pcount > 0 && --fmtcnt >= 0) {
                    if (*fmt == ')')
                        --pcount;
                    else if (*fmt == '(')
                        ++pcount;
                    fmt++;
                }
                keylen = fmt - keystart - 1;
                if (fmtcnt < 0 || pcount > 0) {
                    PyErr_SetString(PyExc_ValueError, "incomplete format key");
                    goto error;
                }
                key = PyString_FromStringAndSize(keystart, keylen);
                if (key == NULL)
                    goto error;
                if (args_owned) {
                    Py_DECREF(args);
                    args_owned = 0;
                }
                args = PyObject_GetItem(dict, key);
                Py_DECREF(key);
                if (args == NULL) {
                    goto error;
                }
                args_owned = 1;
                arglen = -1;
                argidx = -2;
            }
            while (--fmtcnt >= 0) {
                switch (c = *fmt++) {
                    case '-':
                        flags |= F_LJUST;
                        continue;
                    case '+':
                        flags |= F_SIGN;
                        continue;
                    case ' ':
                        flags |= F_BLANK;
                        continue;
                    case '#':
                        flags |= F_ALT;
                        continue;
                    case '0':
                        flags |= F_ZERO;
                        continue;
                }
                break;
            }
            if (c == '*') {
                v = getnextarg(args, arglen, &argidx);
                if (v == NULL)
                    goto error;
                if (!PyInt_Check(v)) {
                    PyErr_SetString(PyExc_TypeError, "* wants int");
                    goto error;
                }
                width = PyInt_AsSsize_t(v);
                if (width == -1 && PyErr_Occurred())
                    goto error;
                if (width < 0) {
                    flags |= F_LJUST;
                    width = -width;
                }
                if (--fmtcnt >= 0)
                    c = *fmt++;
            } else if (c >= 0 && isdigit(c)) {
                width = c - '0';
                while (--fmtcnt >= 0) {
                    c = Py_CHARMASK(*fmt++);
                    if (!isdigit(c))
                        break;
                    if (width > (PY_SSIZE_T_MAX - ((int)c - '0')) / 10) {
                        PyErr_SetString(PyExc_ValueError, "width too big");
                        goto error;
                    }
                    width = width * 10 + (c - '0');
                }
            }
            if (c == '.') {
                prec = 0;
                if (--fmtcnt >= 0)
                    c = *fmt++;
                if (c == '*') {
                    v = getnextarg(args, arglen, &argidx);
                    if (v == NULL)
                        goto error;
                    if (!PyInt_Check(v)) {
                        PyErr_SetString(PyExc_TypeError, "* wants int");
                        goto error;
                    }
                    prec = _PyInt_AsInt(v);
                    if (prec == -1 && PyErr_Occurred())
                        goto error;
                    if (prec < 0)
                        prec = 0;
                    if (--fmtcnt >= 0)
                        c = *fmt++;
                } else if (c >= 0 && isdigit(c)) {
                    prec = c - '0';
                    while (--fmtcnt >= 0) {
                        c = Py_CHARMASK(*fmt++);
                        if (!isdigit(c))
                            break;
                        if (prec > (INT_MAX - ((int)c - '0')) / 10) {
                            PyErr_SetString(PyExc_ValueError, "prec too big");
                            goto error;
                        }
                        prec = prec * 10 + (c - '0');
                    }
                }
            } /* prec */
            if (fmtcnt >= 0) {
                if (c == 'h' || c == 'l' || c == 'L') {
                    if (--fmtcnt >= 0)
                        c = *fmt++;
                }
            }
            if (fmtcnt < 0) {
                PyErr_SetString(PyExc_ValueError, "incomplete format");
                goto error;
            }
            if (c != '%') {
                v = getnextarg(args, arglen, &argidx);
                if (v == NULL)
                    goto error;
            }
            sign = 0;
            fill = ' ';
            switch (c) {
                case '%':
                    pbuf = "%";
                    len = 1;
                    break;
                case 's':
#ifdef Py_USING_UNICODE
                    if (PyUnicode_Check(v)) {
                        fmt = fmt_start;
                        argidx = argidx_start;
                        goto unicode;
                    }
#endif
                    temp = _PyObject_Str(v);
#ifdef Py_USING_UNICODE
                    if (temp != NULL && PyUnicode_Check(temp)) {
                        Py_DECREF(temp);
                        fmt = fmt_start;
                        argidx = argidx_start;
                        goto unicode;
                    }
#endif
                /* Fall through */
                case 'r':
                    if (c == 'r')
                        temp = PyObject_Repr(v);
                    if (temp == NULL)
                        goto error;
                    if (!PyString_Check(temp)) {
                        PyErr_SetString(PyExc_TypeError, "%s argument has non-string str()");
                        Py_DECREF(temp);
                        goto error;
                    }
                    pbuf = PyString_AS_STRING(temp);
                    len = PyString_GET_SIZE(temp);
                    if (prec >= 0 && len > prec)
                        len = prec;
                    break;
                case 'i':
                case 'd':
                case 'u':
                case 'o':
                case 'x':
                case 'X':
                    if (c == 'i')
                        c = 'd';
                    isnumok = 0;
                    if (PyNumber_Check(v)) {
                        PyObject* iobj = NULL;

                        if (PyInt_Check(v) || (PyLong_Check(v))) {
                            iobj = v;
                            Py_INCREF(iobj);
                        } else {
                            iobj = PyNumber_Int(v);
                            if (iobj == NULL) {
                                PyErr_Clear();
                                iobj = PyNumber_Long(v);
                            }
                        }
                        if (iobj != NULL) {
                            if (PyInt_Check(iobj)) {
                                isnumok = 1;
                                pbuf = formatbuf;
                                // Pyston change:
                                len = formatint(formatbuf /* pbuf */, sizeof(formatbuf), flags, prec, c, iobj);
                                Py_DECREF(iobj);
                                if (len < 0)
                                    goto error;
                                sign = 1;
                            } else if (PyLong_Check(iobj)) {
                                int ilen;

                                isnumok = 1;
                                temp = _PyString_FormatLong(iobj, flags, prec, c, &pbuf, &ilen);
                                Py_DECREF(iobj);
                                len = ilen;
                                if (!temp)
                                    goto error;
                                sign = 1;
                            } else {
                                Py_DECREF(iobj);
                            }
                        }
                    }
                    if (!isnumok) {
                        PyErr_Format(PyExc_TypeError, "%%%c format: a number is required, "
                                                      "not %.200s",
                                     c, Py_TYPE(v)->tp_name);
                        goto error;
                    }
                    if (flags & F_ZERO)
                        fill = '0';
                    break;
                case 'e':
                case 'E':
                case 'f':
                case 'F':
                case 'g':
                case 'G':
                    temp = formatfloat(v, flags, prec, c);
                    if (temp == NULL)
                        goto error;
                    pbuf = PyString_AS_STRING(temp);
                    len = PyString_GET_SIZE(temp);
                    sign = 1;
                    if (flags & F_ZERO)
                        fill = '0';
                    break;
                case 'c':
#ifdef Py_USING_UNICODE
                    if (PyUnicode_Check(v)) {
                        fmt = fmt_start;
                        argidx = argidx_start;
                        goto unicode;
                    }
#endif
                    pbuf = formatbuf;
                    // Pyston change:
                    len = formatchar(formatbuf /* was pbuf */, sizeof(formatbuf), v);
                    if (len < 0)
                        goto error;
                    break;
                default:
                    PyErr_Format(PyExc_ValueError, "unsupported format character '%c' (0x%x) "
                                                   "at index %zd",
                                 c, c, (Py_ssize_t)(fmt - 1 - PyString_AsString(format)));
                    goto error;
            }
            if (sign) {
                if (*pbuf == '-' || *pbuf == '+') {
                    sign = *pbuf++;
                    len--;
                } else if (flags & F_SIGN)
                    sign = '+';
                else if (flags & F_BLANK)
                    sign = ' ';
                else
                    sign = 0;
            }
            if (width < len)
                width = len;
            if (rescnt - (sign != 0) < width) {
                reslen -= rescnt;
                rescnt = width + fmtcnt + 100;
                reslen += rescnt;
                if (reslen < 0) {
                    Py_DECREF(result);
                    Py_XDECREF(temp);
                    return PyErr_NoMemory();
                }
                if (_PyString_Resize(&result, reslen)) {
                    Py_XDECREF(temp);
                    return NULL;
                }
                res = PyString_AS_STRING(result) + reslen - rescnt;
            }
            if (sign) {
                if (fill != ' ')
                    *res++ = sign;
                rescnt--;
                if (width > len)
                    width--;
            }
            if ((flags & F_ALT) && (c == 'x' || c == 'X')) {
                assert(pbuf[0] == '0');
                assert(pbuf[1] == c);
                if (fill != ' ') {
                    *res++ = *pbuf++;
                    *res++ = *pbuf++;
                }
                rescnt -= 2;
                width -= 2;
                if (width < 0)
                    width = 0;
                len -= 2;
            }
            if (width > len && !(flags & F_LJUST)) {
                do {
                    --rescnt;
                    *res++ = fill;
                } while (--width > len);
            }
            if (fill == ' ') {
                if (sign)
                    *res++ = sign;
                if ((flags & F_ALT) && (c == 'x' || c == 'X')) {
                    assert(pbuf[0] == '0');
                    assert(pbuf[1] == c);
                    *res++ = *pbuf++;
                    *res++ = *pbuf++;
                }
            }
            Py_MEMCPY(res, pbuf, len);
            res += len;
            rescnt -= len;
            while (--width >= len) {
                --rescnt;
                *res++ = ' ';
            }
            if (dict && (argidx < arglen) && c != '%') {
                PyErr_SetString(PyExc_TypeError, "not all arguments converted during string formatting");
                Py_XDECREF(temp);
                goto error;
            }
            Py_XDECREF(temp);
        } /* '%' */
    }     /* until end */
    if (argidx < arglen && !dict) {
        PyErr_SetString(PyExc_TypeError, "not all arguments converted during string formatting");
        goto error;
    }
    if (args_owned) {
        Py_DECREF(args);
    }
    if (_PyString_Resize(&result, reslen - rescnt))
        return NULL;
    return result;

#ifdef Py_USING_UNICODE
unicode:
    if (args_owned) {
        Py_DECREF(args);
        args_owned = 0;
    }
    /* Fiddle args right (remove the first argidx arguments) */
    if (PyTuple_Check(orig_args) && argidx > 0) {
        PyObject* v;
        Py_ssize_t n = PyTuple_GET_SIZE(orig_args) - argidx;
        v = PyTuple_New(n);
        if (v == NULL)
            goto error;
        while (--n >= 0) {
            PyObject* w = PyTuple_GET_ITEM(orig_args, n + argidx);
            Py_INCREF(w);
            PyTuple_SET_ITEM(v, n, w);
        }
        args = v;
    } else {
        Py_INCREF(orig_args);
        args = orig_args;
    }
    args_owned = 1;
    /* Take what we have of the result and let the Unicode formatting
       function format the rest of the input. */
    rescnt = res - PyString_AS_STRING(result);
    if (_PyString_Resize(&result, rescnt))
        goto error;
    fmtcnt = PyString_GET_SIZE(format) - (fmt - PyString_AS_STRING(format));
    format = PyUnicode_Decode(fmt, fmtcnt, NULL, NULL);
    if (format == NULL)
        goto error;
    v = PyUnicode_Format(format, args);
    Py_DECREF(format);
    if (v == NULL)
        goto error;
    /* Paste what we have (result) to what the Unicode formatting
       function returned (v) and return the result (or error) */
    w = PyUnicode_Concat(result, v);
    Py_DECREF(result);
    Py_DECREF(v);
    Py_DECREF(args);
    return w;
#endif /* Py_USING_UNICODE */

error:
    Py_DECREF(result);
    if (args_owned) {
        Py_DECREF(args);
    }
    return NULL;
}
extern "C" Box* strMod(BoxedString* lhs, Box* rhs) {
    Box* rtn = PyString_Format(lhs, rhs);
    checkAndThrowCAPIException();
    assert(rtn);
    return rtn;
}

extern "C" Box* strMul(BoxedString* lhs, Box* rhs) {
    assert(isSubclass(lhs->cls, str_cls));

    int n;
    if (isSubclass(rhs->cls, int_cls))
        n = static_cast<BoxedInt*>(rhs)->n;
    else
        return NotImplemented;

    // TODO: use createUninitializedString and getWriteableStringContents
    int sz = lhs->s.size();
    std::string buf(sz * n, '\0');
    for (int i = 0; i < n; i++) {
        memcpy(&buf[sz * i], lhs->s.c_str(), sz);
    }
    return new BoxedString(std::move(buf));
}

extern "C" Box* strLt(BoxedString* lhs, Box* rhs) {
    assert(isSubclass(lhs->cls, str_cls));

    if (rhs->cls != str_cls)
        return NotImplemented;

    BoxedString* srhs = static_cast<BoxedString*>(rhs);
    return boxBool(lhs->s < srhs->s);
}

extern "C" Box* strLe(BoxedString* lhs, Box* rhs) {
    assert(isSubclass(lhs->cls, str_cls));

    if (rhs->cls != str_cls)
        return NotImplemented;

    BoxedString* srhs = static_cast<BoxedString*>(rhs);
    return boxBool(lhs->s <= srhs->s);
}

extern "C" Box* strGt(BoxedString* lhs, Box* rhs) {
    assert(isSubclass(lhs->cls, str_cls));

    if (rhs->cls != str_cls)
        return NotImplemented;

    BoxedString* srhs = static_cast<BoxedString*>(rhs);
    return boxBool(lhs->s > srhs->s);
}

extern "C" Box* strGe(BoxedString* lhs, Box* rhs) {
    assert(isSubclass(lhs->cls, str_cls));

    if (rhs->cls != str_cls)
        return NotImplemented;

    BoxedString* srhs = static_cast<BoxedString*>(rhs);
    return boxBool(lhs->s >= srhs->s);
}

extern "C" Box* strEq(BoxedString* lhs, Box* rhs) {
    assert(isSubclass(lhs->cls, str_cls));

    if (rhs->cls != str_cls)
        return NotImplemented;

    BoxedString* srhs = static_cast<BoxedString*>(rhs);
    return boxBool(lhs->s == srhs->s);
}

extern "C" Box* strNe(BoxedString* lhs, Box* rhs) {
    assert(isSubclass(lhs->cls, str_cls));

    if (rhs->cls != str_cls)
        return NotImplemented;

    BoxedString* srhs = static_cast<BoxedString*>(rhs);
    return boxBool(lhs->s != srhs->s);
}

#define JUST_LEFT 0
#define JUST_RIGHT 1
#define JUST_CENTER 2
static Box* pad(BoxedString* self, Box* width, Box* fillchar, int justType) {
    assert(width->cls == int_cls);
    assert(isSubclass(fillchar->cls, str_cls));
    assert(static_cast<BoxedString*>(fillchar)->s.size() == 1);
    int64_t curWidth = self->s.size();
    int64_t targetWidth = static_cast<BoxedInt*>(width)->n;

    if (curWidth >= targetWidth) {
        if (self->cls == str_cls) {
            return self;
        } else {
            // If self isn't a string but a subclass of str, then make a new string to return
            return new BoxedString(self->s);
        }
    }

    char c = static_cast<BoxedString*>(fillchar)->s[0];

    int padLeft, padRight;
    int nNeeded = targetWidth - curWidth;
    switch (justType) {
        case JUST_LEFT:
            padLeft = 0;
            padRight = nNeeded;
            break;
        case JUST_RIGHT:
            padLeft = nNeeded;
            padRight = 0;
            break;
        case JUST_CENTER:
            padLeft = nNeeded / 2 + (nNeeded & targetWidth & 1);
            padRight = nNeeded - padLeft;
            break;
        default:
            abort();
    }

    // TODO this is probably slow
    std::string res = std::string(padLeft, c) + self->s + std::string(padRight, c);

    return new BoxedString(std::move(res));
}
extern "C" Box* strLjust(BoxedString* lhs, Box* width, Box* fillchar) {
    return pad(lhs, width, fillchar, JUST_LEFT);
}
extern "C" Box* strRjust(BoxedString* lhs, Box* width, Box* fillchar) {
    return pad(lhs, width, fillchar, JUST_RIGHT);
}
extern "C" Box* strCenter(BoxedString* lhs, Box* width, Box* fillchar) {
    return pad(lhs, width, fillchar, JUST_CENTER);
}

extern "C" Box* strLen(BoxedString* self) {
    assert(isSubclass(self->cls, str_cls));

    return boxInt(self->s.size());
}

extern "C" Box* strStr(BoxedString* self) {
    assert(isSubclass(self->cls, str_cls));

    return self;
}

static bool _needs_escaping[256]
    = { true,  true,  true,  true,  true,  true,  true,  true,  true,  true,  true,  true,  true,  true,  true,  true,
        true,  true,  true,  true,  true,  true,  true,  true,  true,  true,  true,  true,  true,  true,  true,  true,
        false, false, false, false, false, false, false, true,  false, false, false, false, false, false, false, false,
        false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false,
        false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false,
        false, false, false, false, false, false, false, false, false, false, false, false, true,  false, false, false,
        false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false,
        false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, true,
        true,  true,  true,  true,  true,  true,  true,  true,  true,  true,  true,  true,  true,  true,  true,  true,
        true,  true,  true,  true,  true,  true,  true,  true,  true,  true,  true,  true,  true,  true,  true,  true,
        true,  true,  true,  true,  true,  true,  true,  true,  true,  true,  true,  true,  true,  true,  true,  true,
        true,  true,  true,  true,  true,  true,  true,  true,  true,  true,  true,  true,  true,  true,  true,  true,
        true,  true,  true,  true,  true,  true,  true,  true,  true,  true,  true,  true,  true,  true,  true,  true,
        true,  true,  true,  true,  true,  true,  true,  true,  true,  true,  true,  true,  true,  true,  true,  true,
        true,  true,  true,  true,  true,  true,  true,  true,  true,  true,  true,  true,  true,  true,  true,  true,
        true,  true,  true,  true,  true,  true,  true,  true,  true,  true,  true,  true,  true,  true,  true,  true };
static char _hex[17] = "0123456789abcdef"; // really only needs to be 16 but clang will complain

extern "C" PyObject* PyString_Repr(PyObject* obj, int smartquotes) noexcept {
    BoxedString* self = (BoxedString*)obj;
    assert(isSubclass(self->cls, str_cls));

    std::ostringstream os("");

    const std::string& s = self->s;
    char quote = '\'';
    if (smartquotes && s.find('\'', 0) != std::string::npos && s.find('\"', 0) == std::string::npos) {
        quote = '\"';
    }
    os << quote;
    for (int i = 0; i < s.size(); i++) {
        char c = s[i];
        if ((c == '\'' && quote == '\"') || !_needs_escaping[c & 0xff]) {
            os << c;
        } else {
            char special = 0;
            switch (c) {
                case '\t':
                    special = 't';
                    break;
                case '\n':
                    special = 'n';
                    break;
                case '\r':
                    special = 'r';
                    break;
                case '\'':
                    special = '\'';
                    break;
                case '\"':
                    special = '\"';
                    break;
                case '\\':
                    special = '\\';
                    break;
            }
            if (special) {
                os << '\\';
                os << special;
            } else {
                os << '\\';
                os << 'x';
                os << _hex[(c & 0xff) / 16];
                os << _hex[(c & 0xff) % 16];
            }
        }
    }
    os << quote;

    return boxString(os.str());
}

extern "C" Box* strRepr(BoxedString* self) {
    return PyString_Repr(self, 1 /* smartquotes */);
}

/* Unescape a backslash-escaped string. If unicode is non-zero,
   the string is a u-literal. If recode_encoding is non-zero,
   the string is UTF-8 encoded and should be re-encoded in the
   specified encoding.  */

extern "C" PyObject* PyString_DecodeEscape(const char* s, Py_ssize_t len, const char* errors, Py_ssize_t unicode,
                                           const char* recode_encoding) noexcept {
    int c;
    char* p, *buf;
    const char* end;
    PyObject* v;
    Py_ssize_t newlen = recode_encoding ? 4 * len : len;
    v = PyString_FromStringAndSize((char*)NULL, newlen);
    if (v == NULL)
        return NULL;
    p = buf = PyString_AsString(v);
    end = s + len;
    while (s < end) {
        if (*s != '\\') {
        non_esc:
#ifdef Py_USING_UNICODE
            if (recode_encoding && (*s & 0x80)) {
                PyObject* u, *w;
                char* r;
                const char* t;
                Py_ssize_t rn;
                t = s;
                /* Decode non-ASCII bytes as UTF-8. */
                while (t < end && (*t & 0x80))
                    t++;
                u = PyUnicode_DecodeUTF8(s, t - s, errors);
                if (!u)
                    goto failed;

                /* Recode them in target encoding. */
                w = PyUnicode_AsEncodedString(u, recode_encoding, errors);
                Py_DECREF(u);
                if (!w)
                    goto failed;

                /* Append bytes to output buffer. */
                assert(PyString_Check(w));
                r = PyString_AS_STRING(w);
                rn = PyString_GET_SIZE(w);
                Py_MEMCPY(p, r, rn);
                p += rn;
                Py_DECREF(w);
                s = t;
            } else {
                *p++ = *s++;
            }
#else
            *p++ = *s++;
#endif
            continue;
        }
        s++;
        if (s == end) {
            PyErr_SetString(PyExc_ValueError, "Trailing \\ in string");
            goto failed;
        }
        switch (*s++) {
            /* XXX This assumes ASCII! */
            case '\n':
                break;
            case '\\':
                *p++ = '\\';
                break;
            case '\'':
                *p++ = '\'';
                break;
            case '\"':
                *p++ = '\"';
                break;
            case 'b':
                *p++ = '\b';
                break;
            case 'f':
                *p++ = '\014';
                break; /* FF */
            case 't':
                *p++ = '\t';
                break;
            case 'n':
                *p++ = '\n';
                break;
            case 'r':
                *p++ = '\r';
                break;
            case 'v':
                *p++ = '\013';
                break; /* VT */
            case 'a':
                *p++ = '\007';
                break; /* BEL, not classic C */
            case '0':
            case '1':
            case '2':
            case '3':
            case '4':
            case '5':
            case '6':
            case '7':
                c = s[-1] - '0';
                if (s < end && '0' <= *s && *s <= '7') {
                    c = (c << 3) + *s++ - '0';
                    if (s < end && '0' <= *s && *s <= '7')
                        c = (c << 3) + *s++ - '0';
                }
                *p++ = c;
                break;
            case 'x':
                if (s + 1 < end && isxdigit(Py_CHARMASK(s[0])) && isxdigit(Py_CHARMASK(s[1]))) {
                    unsigned int x = 0;
                    c = Py_CHARMASK(*s);
                    s++;
                    if (isdigit(c))
                        x = c - '0';
                    else if (islower(c))
                        x = 10 + c - 'a';
                    else
                        x = 10 + c - 'A';
                    x = x << 4;
                    c = Py_CHARMASK(*s);
                    s++;
                    if (isdigit(c))
                        x += c - '0';
                    else if (islower(c))
                        x += 10 + c - 'a';
                    else
                        x += 10 + c - 'A';
                    *p++ = x;
                    break;
                }
                if (!errors || strcmp(errors, "strict") == 0) {
                    PyErr_SetString(PyExc_ValueError, "invalid \\x escape");
                    goto failed;
                }
                if (strcmp(errors, "replace") == 0) {
                    *p++ = '?';
                } else if (strcmp(errors, "ignore") == 0)
                    /* do nothing */;
                else {
                    PyErr_Format(PyExc_ValueError, "decoding error; "
                                                   "unknown error handling code: %.400s",
                                 errors);
                    goto failed;
                }
                /* skip \x */
                if (s < end && isxdigit(Py_CHARMASK(s[0])))
                    s++; /* and a hexdigit */
                break;
#ifndef Py_USING_UNICODE
            case 'u':
            case 'U':
            case 'N':
                if (unicode) {
                    PyErr_SetString(PyExc_ValueError, "Unicode escapes not legal "
                                                      "when Unicode disabled");
                    goto failed;
                }
#endif
            default:
                *p++ = '\\';
                s--;
                goto non_esc; /* an arbitrary number of unescaped
                                 UTF-8 bytes may follow. */
        }
    }
    if (p - buf < newlen)
        _PyString_Resize(&v, p - buf); /* v is cleared on error */
    return v;
failed:
    Py_DECREF(v);
    return NULL;
}

extern "C" Box* strHash(BoxedString* self) {
    assert(isSubclass(self->cls, str_cls));

    std::hash<std::string> H;
    return boxInt(H(self->s));
}

extern "C" Box* strNonzero(BoxedString* self) {
    ASSERT(isSubclass(self->cls, str_cls), "%s", self->cls->tp_name);

    return boxBool(self->s.size() != 0);
}

extern "C" Box* strNew(BoxedClass* cls, Box* obj) {
    assert(isSubclass(cls, str_cls));

    Box* rtn = str(obj);
    assert(rtn->cls == str_cls);

    if (cls == str_cls)
        return rtn;

    return new (cls) BoxedString(static_cast<BoxedString*>(rtn)->s);
}

extern "C" Box* basestringNew(BoxedClass* cls, Box* args, Box* kwargs) {
    raiseExcHelper(TypeError, "The basestring type cannot be instantiated");
}

Box* _strSlice(BoxedString* self, i64 start, i64 stop, i64 step, i64 length) {
    assert(isSubclass(self->cls, str_cls));

    const std::string& s = self->s;

    assert(step != 0);
    if (step > 0) {
        assert(0 <= start);
        assert(stop <= s.size());
    } else {
        assert(start < s.size());
        assert(-1 <= stop);
    }

    std::string chars;
    if (length > 0) {
        chars.resize(length);
        copySlice(&chars[0], &s[0], start, step, length);
    }
    return boxString(std::move(chars));
}

Box* strIsAlpha(BoxedString* self) {
    assert(isSubclass(self->cls, str_cls));

    const std::string& str(self->s);
    if (str.empty())
        return False;

    for (const auto& c : str) {
        if (!std::isalpha(c))
            return False;
    }

    return True;
}

Box* strIsDigit(BoxedString* self) {
    assert(isSubclass(self->cls, str_cls));

    const std::string& str(self->s);
    if (str.empty())
        return False;

    for (const auto& c : str) {
        if (!std::isdigit(c))
            return False;
    }

    return True;
}

Box* strIsAlnum(BoxedString* self) {
    assert(isSubclass(self->cls, str_cls));

    const std::string& str(self->s);
    if (str.empty())
        return False;

    for (const auto& c : str) {
        if (!std::isalnum(c))
            return False;
    }

    return True;
}

Box* strIsLower(BoxedString* self) {
    assert(isSubclass(self->cls, str_cls));

    const std::string& str(self->s);
    bool lowered = false;

    if (str.empty())
        return False;

    for (const auto& c : str) {
        if (std::isspace(c) || std::isdigit(c)) {
            continue;
        } else if (!std::islower(c)) {
            return False;
        } else {
            lowered = true;
        }
    }

    return boxBool(lowered);
}

Box* strIsUpper(BoxedString* self) {
    assert(isSubclass(self->cls, str_cls));

    const std::string& str(self->s);

    if (str.empty())
        return False;

    bool cased = false;
    for (const auto& c : str) {
        if (std::islower(c))
            return False;
        else if (!cased && isupper(c))
            cased = true;
    }

    return boxBool(cased);
}

Box* strIsSpace(BoxedString* self) {
    assert(isSubclass(self->cls, str_cls));

    const std::string& str(self->s);
    if (str.empty())
        return False;

    for (const auto& c : str) {
        if (!std::isspace(c))
            return False;
    }

    return True;
}

Box* strIsTitle(BoxedString* self) {
    assert(isSubclass(self->cls, str_cls));

    const std::string& str(self->s);

    if (str.empty())
        return False;
    if (str.size() == 1)
        return boxBool(std::isupper(str[0]));

    bool cased = false, start_of_word = true;

    for (const auto& c : str) {
        if (std::isupper(c)) {
            if (!start_of_word) {
                return False;
            }

            start_of_word = false;
            cased = true;
        } else if (std::islower(c)) {
            if (start_of_word) {
                return False;
            }

            start_of_word = false;
            cased = true;
        } else {
            start_of_word = true;
        }
    }

    return boxBool(cased);
}

Box* strJoin(BoxedString* self, Box* rhs) {
    assert(isSubclass(self->cls, str_cls));

    std::string output_str;
    llvm::raw_string_ostream os(output_str);
    int i = 0;
    for (Box* e : rhs->pyElements()) {
        if (i > 0)
            os << self->s;
        os << str(e)->s;
        ++i;
    }
    os.flush();
    return boxString(std::move(output_str));
}

extern "C" PyObject* _PyString_Join(PyObject* sep, PyObject* x) noexcept {
    try {
        RELEASE_ASSERT(isSubclass(sep->cls, str_cls), "");
        return strJoin((BoxedString*)sep, x);
    } catch (ExcInfo e) {
        setCAPIException(e);
        return NULL;
    }
}

Box* strReplace(Box* _self, Box* _old, Box* _new, Box** _args) {
    if (_self->cls != str_cls)
        raiseExcHelper(TypeError, "descriptor 'replace' requires a 'str' object but received a '%s'",
                       getTypeName(_self));
    BoxedString* self = static_cast<BoxedString*>(_self);

#ifdef Py_USING_UNICODE
    if (PyUnicode_Check(_old) || PyUnicode_Check(_new))
        return PyUnicode_Replace((PyObject*)self, _old, _new, -1 /*count*/);
#endif

    if (_old->cls != str_cls)
        raiseExcHelper(TypeError, "expected a character buffer object");
    BoxedString* old = static_cast<BoxedString*>(_old);

    if (_new->cls != str_cls)
        raiseExcHelper(TypeError, "expected a character buffer object");
    BoxedString* new_ = static_cast<BoxedString*>(_new);

    Box* _maxreplace = _args[0];
    if (!isSubclass(_maxreplace->cls, int_cls))
        raiseExcHelper(TypeError, "an integer is required");

    int max_replaces = static_cast<BoxedInt*>(_maxreplace)->n;
    size_t start_pos = 0;
    std::string s = self->s;
    for (int num_replaced = 0; num_replaced < max_replaces || max_replaces < 0; ++num_replaced) {
        start_pos = s.find(old->s, start_pos);
        if (start_pos == std::string::npos)
            break;
        s.replace(start_pos, old->s.length(), new_->s);
        start_pos += new_->s.length(); // Handles case where 'to' is a substring of 'from'
    }
    return new BoxedString(std::move(s));
}

Box* strPartition(BoxedString* self, BoxedString* sep) {
    RELEASE_ASSERT(isSubclass(self->cls, str_cls), "");
    RELEASE_ASSERT(isSubclass(sep->cls, str_cls), "");

    size_t found_idx = self->s.find(sep->s);
    if (found_idx == std::string::npos)
        return new BoxedTuple({ self, boxStrConstant(""), boxStrConstant("") });


    return new BoxedTuple({ boxStrConstantSize(self->s.c_str(), found_idx),
                            boxStrConstantSize(self->s.c_str() + found_idx, sep->s.size()),
                            boxStrConstantSize(self->s.c_str() + found_idx + sep->s.size(),
                                               self->s.size() - found_idx - sep->s.size()) });
}

Box* strRpartition(BoxedString* self, BoxedString* sep) {
    RELEASE_ASSERT(isSubclass(self->cls, str_cls), "");
    RELEASE_ASSERT(isSubclass(sep->cls, str_cls), "");

    size_t found_idx = self->s.rfind(sep->s);
    if (found_idx == std::string::npos)
        return new BoxedTuple({ self, boxStrConstant(""), boxStrConstant("") });


    return new BoxedTuple({ boxStrConstantSize(self->s.c_str(), found_idx),
                            boxStrConstantSize(self->s.c_str() + found_idx, sep->s.size()),
                            boxStrConstantSize(self->s.c_str() + found_idx + sep->s.size(),
                                               self->s.size() - found_idx - sep->s.size()) });
}

extern "C" PyObject* _do_string_format(PyObject* self, PyObject* args, PyObject* kwargs);

Box* strFormat(BoxedString* self, BoxedTuple* args, BoxedDict* kwargs) {
    assert(args->cls == tuple_cls);
    assert(kwargs->cls == dict_cls);

    Box* rtn = _do_string_format(self, args, kwargs);
    checkAndThrowCAPIException();
    assert(rtn);
    return rtn;
}

Box* strStrip(BoxedString* self, Box* chars) {
    assert(isSubclass(self->cls, str_cls));

    if (isSubclass(chars->cls, str_cls)) {
        return new BoxedString(llvm::StringRef(self->s).trim(static_cast<BoxedString*>(chars)->s));
    } else if (chars->cls == none_cls) {
        return new BoxedString(llvm::StringRef(self->s).trim(" \t\n\r\f\v"));
    } else {
        raiseExcHelper(TypeError, "strip arg must be None, str or unicode");
    }
}

Box* strLStrip(BoxedString* self, Box* chars) {
    assert(isSubclass(self->cls, str_cls));

    if (isSubclass(chars->cls, str_cls)) {
        return new BoxedString(llvm::StringRef(self->s).ltrim(static_cast<BoxedString*>(chars)->s));
    } else if (chars->cls == none_cls) {
        return new BoxedString(llvm::StringRef(self->s).ltrim(" \t\n\r\f\v"));
    } else {
        raiseExcHelper(TypeError, "lstrip arg must be None, str or unicode");
    }
}

Box* strRStrip(BoxedString* self, Box* chars) {
    assert(isSubclass(self->cls, str_cls));

    if (isSubclass(chars->cls, str_cls)) {
        return new BoxedString(llvm::StringRef(self->s).rtrim(static_cast<BoxedString*>(chars)->s));
    } else if (chars->cls == none_cls) {
        return new BoxedString(llvm::StringRef(self->s).rtrim(" \t\n\r\f\v"));
    } else {
        raiseExcHelper(TypeError, "rstrip arg must be None, str or unicode");
    }
}

Box* strCapitalize(BoxedString* self) {
    assert(isSubclass(self->cls, str_cls));

    std::string s(self->s);

    for (auto& i : s) {
        i = std::tolower(i);
    }

    if (!s.empty()) {
        s[0] = std::toupper(s[0]);
    }

    return boxString(s);
}

Box* strTitle(BoxedString* self) {
    assert(isSubclass(self->cls, str_cls));

    std::string s(self->s);
    bool start_of_word = false;

    for (auto& i : s) {
        if (std::islower(i)) {
            if (!start_of_word) {
                i = std::toupper(i);
            }
            start_of_word = true;
        } else if (std::isupper(i)) {
            if (start_of_word) {
                i = std::tolower(i);
            }
            start_of_word = true;
        } else {
            start_of_word = false;
        }
    }
    return boxString(s);
}

Box* strTranslate(BoxedString* self, BoxedString* table, BoxedString* delete_chars) {
    if (self->cls != str_cls)
        raiseExcHelper(TypeError, "descriptor 'translate' requires a 'str' object but received a '%s'",
                       getTypeName(self));

    std::unordered_set<char> delete_set;
    if (delete_chars) {
        if (delete_chars->cls != str_cls)
            raiseExcHelper(TypeError, "expected a character buffer object");
        delete_set.insert(delete_chars->s.begin(), delete_chars->s.end());
    }

    bool have_table = table != None;
    if (have_table) {
        if (table->cls != str_cls)
            raiseExcHelper(TypeError, "expected a character buffer object");
        if (table->s.size() != 256)
            raiseExcHelper(ValueError, "translation table must be 256 characters long");
    }

    std::string str;
    for (const char c : self->s) {
        if (!delete_set.count(c))
            str.append(1, have_table ? table->s[(unsigned char)c] : c);
    }
    return boxString(std::move(str));
}

Box* strLower(BoxedString* self) {
    assert(isSubclass(self->cls, str_cls));
    return boxString(llvm::StringRef(self->s).lower());
}

Box* strUpper(BoxedString* self) {
    assert(isSubclass(self->cls, str_cls));
    return boxString(llvm::StringRef(self->s).upper());
}

Box* strSwapcase(BoxedString* self) {
    std::string s(self->s);

    for (auto& i : s) {
        if (std::islower(i))
            i = std::toupper(i);
        else if (std::isupper(i))
            i = std::tolower(i);
    }

    return boxString(s);
}

Box* strContains(BoxedString* self, Box* elt) {
    assert(isSubclass(self->cls, str_cls));

    if (PyUnicode_Check(elt)) {
        int r = PyUnicode_Contains(self, elt);
        if (r < 0)
            throwCAPIException();
        return boxBool(r);
    }

    if (elt->cls != str_cls)
        raiseExcHelper(TypeError, "'in <string>' requires string as left operand, not %s", getTypeName(elt));

    BoxedString* sub = static_cast<BoxedString*>(elt);

    size_t found_idx = self->s.find(sub->s);
    if (found_idx == std::string::npos)
        return False;
    return True;
}

Box* strStartswith(BoxedString* self, Box* elt, Box* start, Box** _args) {
    Box* end = _args[0];

    if (self->cls != str_cls)
        raiseExcHelper(TypeError, "descriptor 'startswith' requires a 'str' object but received a '%s'",
                       getTypeName(self));

    Py_ssize_t istart = 0, iend = PY_SSIZE_T_MAX;
    if (start) {
        int r = _PyEval_SliceIndex(start, &istart);
        if (!r)
            throwCAPIException();
    }

    if (end) {
        int r = _PyEval_SliceIndex(end, &iend);
        if (!r)
            throwCAPIException();
    }

    if (isSubclass(elt->cls, tuple_cls)) {
        for (auto e : static_cast<BoxedTuple*>(elt)->elts) {
            auto b = strStartswith(self, e, start, _args);
            assert(b->cls == bool_cls);
            if (b == True)
                return True;
        }
        return False;
    }

    if (isSubclass(elt->cls, unicode_cls)) {
        int r = PyUnicode_Tailmatch(self, elt, istart, iend, -1);
        if (r < 0)
            throwCAPIException();
        assert(r == 0 || r == 1);
        return boxBool(r);
    }

    if (elt->cls != str_cls)
        raiseExcHelper(TypeError, "expected a character buffer object");

    BoxedString* sub = static_cast<BoxedString*>(elt);

    Py_ssize_t n = self->s.size();
    iend = std::min(iend, n);
    if (iend < 0)
        iend += n;
    if (iend < 0)
        iend = 0;

    if (istart < 0)
        istart += n;
    if (istart < 0)
        istart = 0;

    Py_ssize_t compare_len = iend - istart;
    if (compare_len < 0)
        return False;
    if (sub->s.size() > compare_len)
        return False;
    return boxBool(self->s.compare(istart, sub->s.size(), sub->s) == 0);
}

Box* strEndswith(BoxedString* self, Box* elt, Box* start, Box** _args) {
    Box* end = _args[0];

    if (self->cls != str_cls)
        raiseExcHelper(TypeError, "descriptor 'endswith' requires a 'str' object but received a '%s'",
                       getTypeName(self));

    Py_ssize_t istart = 0, iend = PY_SSIZE_T_MAX;
    if (start) {
        int r = _PyEval_SliceIndex(start, &istart);
        if (!r)
            throwCAPIException();
    }

    if (end) {
        int r = _PyEval_SliceIndex(end, &iend);
        if (!r)
            throwCAPIException();
    }

    if (isSubclass(elt->cls, unicode_cls)) {
        int r = PyUnicode_Tailmatch(self, elt, istart, iend, +1);
        if (r < 0)
            throwCAPIException();
        assert(r == 0 || r == 1);
        return boxBool(r);
    }

    if (isSubclass(elt->cls, tuple_cls)) {
        for (auto e : static_cast<BoxedTuple*>(elt)->elts) {
            auto b = strEndswith(self, e, start, _args);
            assert(b->cls == bool_cls);
            if (b == True)
                return True;
        }
        return False;
    }

    if (elt->cls != str_cls)
        raiseExcHelper(TypeError, "expected a character buffer object");

    BoxedString* sub = static_cast<BoxedString*>(elt);

    Py_ssize_t n = self->s.size();
    iend = std::min(iend, n);
    if (iend < 0)
        iend += n;
    if (iend < 0)
        iend = 0;

    if (istart < 0)
        istart += n;
    if (istart < 0)
        istart = 0;

    Py_ssize_t compare_len = iend - istart;
    if (compare_len < 0)
        return False;
    if (sub->s.size() > compare_len)
        return False;
    // XXX: this line is the only difference between startswith and endswith:
    istart += compare_len - sub->s.size();
    return boxBool(self->s.compare(istart, sub->s.size(), sub->s) == 0);
}

Box* strDecode(BoxedString* self, Box* encoding, Box* error) {
    if (self->cls != str_cls)
        raiseExcHelper(TypeError, "descriptor 'decode' requires a 'str' object but received a '%s'", getTypeName(self));

    BoxedString* encoding_str = (BoxedString*)encoding;
    BoxedString* error_str = (BoxedString*)error;

    if (encoding_str && encoding_str->cls == unicode_cls)
        encoding_str = (BoxedString*)_PyUnicode_AsDefaultEncodedString(encoding_str, NULL);

    if (encoding_str && encoding_str->cls != str_cls)
        raiseExcHelper(TypeError, "decode() argument 1 must be string, not '%s'", getTypeName(encoding_str));

    if (error_str && error_str->cls == unicode_cls)
        error_str = (BoxedString*)_PyUnicode_AsDefaultEncodedString(error_str, NULL);

    if (error_str && error_str->cls != str_cls)
        raiseExcHelper(TypeError, "decode() argument 2 must be string, not '%s'", getTypeName(error_str));

    Box* result
        = PyCodec_Decode(self, encoding_str ? encoding_str->s.c_str() : NULL, error_str ? error_str->s.c_str() : NULL);
    checkAndThrowCAPIException();
    return result;
}

Box* strEncode(BoxedString* self, Box* encoding, Box* error) {
    if (self->cls != str_cls)
        raiseExcHelper(TypeError, "descriptor 'encode' requires a 'str' object but received a '%s'", getTypeName(self));

    BoxedString* encoding_str = (BoxedString*)encoding;
    BoxedString* error_str = (BoxedString*)error;

    if (encoding_str && encoding_str->cls == unicode_cls)
        encoding_str = (BoxedString*)_PyUnicode_AsDefaultEncodedString(encoding_str, NULL);

    if (encoding_str && encoding_str->cls != str_cls)
        raiseExcHelper(TypeError, "encode() argument 1 must be string, not '%s'", getTypeName(encoding_str));

    if (error_str && error_str->cls == unicode_cls)
        error_str = (BoxedString*)_PyUnicode_AsDefaultEncodedString(error_str, NULL);

    if (error_str && error_str->cls != str_cls)
        raiseExcHelper(TypeError, "encode() argument 2 must be string, not '%s'", getTypeName(error_str));

    Box* result
        = PyCodec_Encode(self, encoding_str ? encoding_str->s.c_str() : NULL, error_str ? error_str->s.c_str() : NULL);
    checkAndThrowCAPIException();
    return result;
}

extern "C" Box* strGetitem(BoxedString* self, Box* slice) {
    assert(isSubclass(self->cls, str_cls));

    if (isSubclass(slice->cls, int_cls)) {
        BoxedInt* islice = static_cast<BoxedInt*>(slice);
        int64_t n = islice->n;
        int size = self->s.size();
        if (n < 0)
            n = size + n;

        if (n < 0 || n >= size) {
            raiseExcHelper(IndexError, "string index out of range");
        }

        char c = self->s[n];
        return new BoxedString(std::string(1, c));
    } else if (slice->cls == slice_cls) {
        BoxedSlice* sslice = static_cast<BoxedSlice*>(slice);

        i64 start, stop, step, length;
        parseSlice(sslice, self->s.size(), &start, &stop, &step, &length);
        return _strSlice(self, start, stop, step, length);
    } else {
        raiseExcHelper(TypeError, "string indices must be integers, not %s", getTypeName(slice));
    }
}


// TODO it looks like strings don't have their own iterators, but instead
// rely on the sequence iteration protocol.
// Should probably implement that, and maybe once that's implemented get
// rid of the striterator class?
BoxedClass* str_iterator_cls = NULL;

class BoxedStringIterator : public Box {
public:
    BoxedString* s;
    std::string::const_iterator it, end;

    BoxedStringIterator(BoxedString* s) : s(s), it(s->s.begin()), end(s->s.end()) {}

    DEFAULT_CLASS(str_iterator_cls);

    static bool hasnextUnboxed(BoxedStringIterator* self) {
        assert(self->cls == str_iterator_cls);
        return self->it != self->end;
    }

    static Box* hasnext(BoxedStringIterator* self) {
        assert(self->cls == str_iterator_cls);
        return boxBool(self->it != self->end);
    }

    static Box* next(BoxedStringIterator* self) {
        assert(self->cls == str_iterator_cls);
        assert(hasnextUnboxed(self));

        char c = *self->it;
        ++self->it;
        return new BoxedString(std::string(1, c));
    }
};

extern "C" void strIteratorGCHandler(GCVisitor* v, Box* b) {
    boxGCHandler(v, b);
    BoxedStringIterator* it = (BoxedStringIterator*)b;
    v->visit(it->s);
}

Box* strIter(BoxedString* self) {
    assert(isSubclass(self->cls, str_cls));
    return new BoxedStringIterator(self);
}

int64_t strCount2Unboxed(BoxedString* self, Box* elt) {
    assert(isSubclass(self->cls, str_cls));

    if (elt->cls != str_cls)
        raiseExcHelper(TypeError, "expected a character buffer object");

    const std::string& s = self->s;
    const std::string& pattern = static_cast<BoxedString*>(elt)->s;

    int found = 0;
    size_t start = 0;
    while (start < s.size()) {
        size_t next = s.find(pattern, start);
        if (next == std::string::npos)
            break;

        found++;
        start = next + pattern.size();
    }
    return found;
}

Box* strCount2(BoxedString* self, Box* elt) {
    return boxInt(strCount2Unboxed(self, elt));
}

Box* strIndex(BoxedString* self, Box* elt) {
    assert(isSubclass(self->cls, str_cls));

    if (elt->cls != str_cls)
        raiseExcHelper(TypeError, "expected a character buffer object");

    const std::string& s = self->s;
    const std::string& pattern = static_cast<BoxedString*>(elt)->s;

    size_t idx = s.find(pattern, 0);
    if (idx == std::string::npos)
        raiseExcHelper(ValueError, "substring not found");

    return boxInt(idx);
}

extern "C" PyObject* PyString_FromString(const char* s) noexcept {
    return boxStrConstant(s);
}

extern "C" int PyString_AsStringAndSize(register PyObject* obj, register char** s, register Py_ssize_t* len) noexcept {
    if (s == NULL) {
        PyErr_BadInternalCall();
        return -1;
    }

    if (!PyString_Check(obj)) {
#ifdef Py_USING_UNICODE
        if (PyUnicode_Check(obj)) {
            obj = _PyUnicode_AsDefaultEncodedString(obj, NULL);
            if (obj == NULL)
                return -1;
        } else
#endif
        {
            PyErr_Format(PyExc_TypeError, "expected string or Unicode object, "
                                          "%.200s found",
                         Py_TYPE(obj)->tp_name);
            return -1;
        }
    }

    *s = PyString_AS_STRING(obj);
    if (len != NULL)
        *len = PyString_GET_SIZE(obj);
    else if (strlen(*s) != (size_t)PyString_GET_SIZE(obj)) {
        PyErr_SetString(PyExc_TypeError, "expected string without null bytes");
        return -1;
    }
    return 0;
}

BoxedString* createUninitializedString(ssize_t n) {
    // I *think* this should avoid doing any copies, by using move constructors:
    return new BoxedString(std::string(n, '\x00'));
}

char* getWriteableStringContents(BoxedString* s) {
    // After doing some reading, I think this is ok:
    // http://stackoverflow.com/questions/14290795/why-is-modifying-a-string-through-a-retrieved-pointer-to-its-data-not-allowed
    // In C++11, std::string is required to store its data contiguously.
    // It looks like it's also required to make it available to write via the [] operator.
    // - Taking a look at GCC's libstdc++, calling operator[] on a non-const string will return
    //   a writeable reference, and "unshare" the string.
    // So surprisingly, this looks ok!
    return &s->s[0];
}

extern "C" PyObject* PyString_FromStringAndSize(const char* s, ssize_t n) noexcept {
    if (s == NULL)
        return createUninitializedString(n);
    return boxStrConstantSize(s, n);
}

extern "C" char* PyString_AsString(PyObject* o) noexcept {
    RELEASE_ASSERT(isSubclass(o->cls, str_cls), "");

    BoxedString* s = static_cast<BoxedString*>(o);
    return getWriteableStringContents(s);
}

extern "C" Py_ssize_t PyString_Size(PyObject* op) noexcept {
    if (isSubclass(op->cls, str_cls))
        return static_cast<BoxedString*>(op)->s.size();

    char* _s;
    Py_ssize_t len;
    if (PyString_AsStringAndSize(op, &_s, &len))
        return -1;
    return len;
}

extern "C" int _PyString_Resize(PyObject** pv, Py_ssize_t newsize) noexcept {
    // This is only allowed to be called when there is only one user of the string (ie a refcount of 1 in CPython)

    assert(pv);
    assert(isSubclass((*pv)->cls, str_cls));
    BoxedString* s = static_cast<BoxedString*>(*pv);
    s->s.resize(newsize, '\0');
    return 0;
}

extern "C" void PyString_Concat(register PyObject** pv, register PyObject* w) noexcept {
    try {
        if (*pv == NULL)
            return;

        if (w == NULL || !PyString_Check(*pv)) {
            *pv = NULL;
            return;
        }

        *pv = strAdd((BoxedString*)*pv, w);
    } catch (ExcInfo e) {
        setCAPIException(e);
        *pv = NULL;
    }
}

extern "C" void PyString_ConcatAndDel(register PyObject** pv, register PyObject* w) noexcept {
    PyString_Concat(pv, w);
}

static PyObject* string_expandtabs(PyStringObject* self, PyObject* args) noexcept {
    const char* e, *p, *qe;
    char* q;
    Py_ssize_t i, j, incr;
    PyObject* u;
    int tabsize = 8;

    if (!PyArg_ParseTuple(args, "|i:expandtabs", &tabsize))
        return NULL;

    /* First pass: determine size of output string */
    i = 0; /* chars up to and including most recent \n or \r */
    j = 0; /* chars since most recent \n or \r (use in tab calculations) */
    e = PyString_AS_STRING(self) + PyString_GET_SIZE(self); /* end of input */
    for (p = PyString_AS_STRING(self); p < e; p++) {
        if (*p == '\t') {
            if (tabsize > 0) {
                incr = tabsize - (j % tabsize);
                if (j > PY_SSIZE_T_MAX - incr)
                    goto overflow1;
                j += incr;
            }
        } else {
            if (j > PY_SSIZE_T_MAX - 1)
                goto overflow1;
            j++;
            if (*p == '\n' || *p == '\r') {
                if (i > PY_SSIZE_T_MAX - j)
                    goto overflow1;
                i += j;
                j = 0;
            }
        }
    }

    if (i > PY_SSIZE_T_MAX - j)
        goto overflow1;

    /* Second pass: create output string and fill it */
    u = PyString_FromStringAndSize(NULL, i + j);
    if (!u)
        return NULL;

    j = 0;                                             /* same as in first pass */
    q = PyString_AS_STRING(u);                         /* next output char */
    qe = PyString_AS_STRING(u) + PyString_GET_SIZE(u); /* end of output */

    for (p = PyString_AS_STRING(self); p < e; p++) {
        if (*p == '\t') {
            if (tabsize > 0) {
                i = tabsize - (j % tabsize);
                j += i;
                while (i--) {
                    if (q >= qe)
                        goto overflow2;
                    *q++ = ' ';
                }
            }
        } else {
            if (q >= qe)
                goto overflow2;
            *q++ = *p;
            j++;
            if (*p == '\n' || *p == '\r')
                j = 0;
        }
    }

    return u;

overflow2:
    Py_DECREF(u);
overflow1:
    PyErr_SetString(PyExc_OverflowError, "new string is too long");
    return NULL;
}

static Py_ssize_t string_buffer_getreadbuf(PyObject* self, Py_ssize_t index, const void** ptr) noexcept {
    RELEASE_ASSERT(index == 0, "");
    // I think maybe this can just be a non-release assert?  shouldn't be able to call this with
    // the wrong type
    RELEASE_ASSERT(isSubclass(self->cls, str_cls), "");

    auto s = static_cast<BoxedString*>(self);
    *ptr = s->s.c_str();
    return s->s.size();
}

static Py_ssize_t string_buffer_getsegcount(PyObject* o, Py_ssize_t* lenp) noexcept {
    RELEASE_ASSERT(lenp == NULL, "");
    RELEASE_ASSERT(isSubclass(o->cls, str_cls), "");

    return 1;
}

static Py_ssize_t string_buffer_getcharbuf(PyStringObject* self, Py_ssize_t index, const char** ptr) {
    if (index != 0) {
        PyErr_SetString(PyExc_SystemError, "accessing non-existent string segment");
        return -1;
    }
    return string_buffer_getreadbuf((PyObject*)self, index, (const void**)ptr);
}

static int string_buffer_getbuffer(BoxedString* self, Py_buffer* view, int flags) noexcept {
    assert(isSubclass(self->cls, str_cls));
    return PyBuffer_FillInfo(view, (PyObject*)self, &self->s[0], self->s.size(), 1, flags);
}

static PyBufferProcs string_as_buffer = {
    (readbufferproc)string_buffer_getreadbuf, // comments are the only way I've found of
    (writebufferproc)NULL,                    // forcing clang-format to break these onto multiple lines
    (segcountproc)string_buffer_getsegcount,  //
    (charbufferproc)string_buffer_getcharbuf, //
    (getbufferproc)string_buffer_getbuffer,   //
    (releasebufferproc)NULL,
};

void strDestructor(Box* b) {
    assert(isSubclass(b->cls, str_cls));
    BoxedString* self = static_cast<BoxedString*>(b);

    self->s.~basic_string();
}

static PyMethodDef string_methods[] = {
    { "split", (PyCFunction)string_split, METH_VARARGS, NULL },
    { "rsplit", (PyCFunction)string_rsplit, METH_VARARGS, NULL },
    { "find", (PyCFunction)string_find, METH_VARARGS, NULL },
    { "rfind", (PyCFunction)string_rfind, METH_VARARGS, NULL },
    { "expandtabs", (PyCFunction)string_expandtabs, METH_VARARGS, NULL },
};

void setupStr() {
    str_cls->simple_destructor = strDestructor;
    str_cls->tp_flags |= Py_TPFLAGS_HAVE_NEWBUFFER;

    str_iterator_cls = BoxedHeapClass::create(type_cls, object_cls, &strIteratorGCHandler, 0, 0,
                                              sizeof(BoxedStringIterator), false, "striterator");
    str_iterator_cls->giveAttr("__hasnext__",
                               new BoxedFunction(boxRTFunction((void*)BoxedStringIterator::hasnext, BOXED_BOOL, 1)));
    str_iterator_cls->giveAttr("next", new BoxedFunction(boxRTFunction((void*)BoxedStringIterator::next, STR, 1)));
    str_iterator_cls->freeze();

    str_cls->tp_as_buffer = &string_as_buffer;

    str_cls->giveAttr("__len__", new BoxedFunction(boxRTFunction((void*)strLen, BOXED_INT, 1)));
    str_cls->giveAttr("__str__", new BoxedFunction(boxRTFunction((void*)strStr, STR, 1)));
    str_cls->giveAttr("__repr__", new BoxedFunction(boxRTFunction((void*)strRepr, STR, 1)));
    str_cls->giveAttr("__hash__", new BoxedFunction(boxRTFunction((void*)strHash, BOXED_INT, 1)));
    str_cls->giveAttr("__nonzero__", new BoxedFunction(boxRTFunction((void*)strNonzero, BOXED_BOOL, 1)));

    str_cls->giveAttr("isalnum", new BoxedFunction(boxRTFunction((void*)strIsAlnum, BOXED_BOOL, 1)));
    str_cls->giveAttr("isalpha", new BoxedFunction(boxRTFunction((void*)strIsAlpha, BOXED_BOOL, 1)));
    str_cls->giveAttr("isdigit", new BoxedFunction(boxRTFunction((void*)strIsDigit, BOXED_BOOL, 1)));
    str_cls->giveAttr("islower", new BoxedFunction(boxRTFunction((void*)strIsLower, BOXED_BOOL, 1)));
    str_cls->giveAttr("isspace", new BoxedFunction(boxRTFunction((void*)strIsSpace, BOXED_BOOL, 1)));
    str_cls->giveAttr("istitle", new BoxedFunction(boxRTFunction((void*)strIsTitle, BOXED_BOOL, 1)));
    str_cls->giveAttr("isupper", new BoxedFunction(boxRTFunction((void*)strIsUpper, BOXED_BOOL, 1)));

    str_cls->giveAttr("decode",
                      new BoxedFunction(boxRTFunction((void*)strDecode, UNKNOWN, 3, 2, false, false), { 0, 0 }));
    str_cls->giveAttr("encode",
                      new BoxedFunction(boxRTFunction((void*)strEncode, UNKNOWN, 3, 2, false, false), { 0, 0 }));

    str_cls->giveAttr("lower", new BoxedFunction(boxRTFunction((void*)strLower, STR, 1)));
    str_cls->giveAttr("swapcase", new BoxedFunction(boxRTFunction((void*)strSwapcase, STR, 1)));
    str_cls->giveAttr("upper", new BoxedFunction(boxRTFunction((void*)strUpper, STR, 1)));

    str_cls->giveAttr("strip", new BoxedFunction(boxRTFunction((void*)strStrip, STR, 2, 1, false, false), { None }));
    str_cls->giveAttr("lstrip", new BoxedFunction(boxRTFunction((void*)strLStrip, STR, 2, 1, false, false), { None }));
    str_cls->giveAttr("rstrip", new BoxedFunction(boxRTFunction((void*)strRStrip, STR, 2, 1, false, false), { None }));

    str_cls->giveAttr("capitalize", new BoxedFunction(boxRTFunction((void*)strCapitalize, STR, 1)));
    str_cls->giveAttr("title", new BoxedFunction(boxRTFunction((void*)strTitle, STR, 1)));

    str_cls->giveAttr("translate",
                      new BoxedFunction(boxRTFunction((void*)strTranslate, STR, 3, 1, false, false), { NULL }));

    str_cls->giveAttr("__contains__", new BoxedFunction(boxRTFunction((void*)strContains, BOXED_BOOL, 2)));

    str_cls->giveAttr("startswith",
                      new BoxedFunction(boxRTFunction((void*)strStartswith, BOXED_BOOL, 4, 2, 0, 0), { NULL, NULL }));
    str_cls->giveAttr("endswith",
                      new BoxedFunction(boxRTFunction((void*)strEndswith, BOXED_BOOL, 4, 2, 0, 0), { NULL, NULL }));

    str_cls->giveAttr("partition", new BoxedFunction(boxRTFunction((void*)strPartition, UNKNOWN, 2)));
    str_cls->giveAttr("rpartition", new BoxedFunction(boxRTFunction((void*)strRpartition, UNKNOWN, 2)));

    str_cls->giveAttr("format", new BoxedFunction(boxRTFunction((void*)strFormat, UNKNOWN, 1, 0, true, true)));

    str_cls->giveAttr("__add__", new BoxedFunction(boxRTFunction((void*)strAdd, UNKNOWN, 2)));
    str_cls->giveAttr("__mod__", new BoxedFunction(boxRTFunction((void*)strMod, UNKNOWN, 2)));
    str_cls->giveAttr("__mul__", new BoxedFunction(boxRTFunction((void*)strMul, UNKNOWN, 2)));
    // TODO not sure if this is right in all cases:
    str_cls->giveAttr("__rmul__", new BoxedFunction(boxRTFunction((void*)strMul, UNKNOWN, 2)));

    str_cls->giveAttr("__lt__", new BoxedFunction(boxRTFunction((void*)strLt, UNKNOWN, 2)));
    str_cls->giveAttr("__le__", new BoxedFunction(boxRTFunction((void*)strLe, UNKNOWN, 2)));
    str_cls->giveAttr("__gt__", new BoxedFunction(boxRTFunction((void*)strGt, UNKNOWN, 2)));
    str_cls->giveAttr("__ge__", new BoxedFunction(boxRTFunction((void*)strGe, UNKNOWN, 2)));
    str_cls->giveAttr("__eq__", new BoxedFunction(boxRTFunction((void*)strEq, UNKNOWN, 2)));
    str_cls->giveAttr("__ne__", new BoxedFunction(boxRTFunction((void*)strNe, UNKNOWN, 2)));

    BoxedString* spaceChar = new BoxedString(" ");
    str_cls->giveAttr("ljust",
                      new BoxedFunction(boxRTFunction((void*)strLjust, UNKNOWN, 3, 1, false, false), { spaceChar }));
    str_cls->giveAttr("rjust",
                      new BoxedFunction(boxRTFunction((void*)strRjust, UNKNOWN, 3, 1, false, false), { spaceChar }));
    str_cls->giveAttr("center",
                      new BoxedFunction(boxRTFunction((void*)strCenter, UNKNOWN, 3, 1, false, false), { spaceChar }));

    str_cls->giveAttr("__getitem__", new BoxedFunction(boxRTFunction((void*)strGetitem, STR, 2)));

    str_cls->giveAttr("__iter__", new BoxedFunction(boxRTFunction((void*)strIter, typeFromClass(str_iterator_cls), 1)));

    str_cls->giveAttr("join", new BoxedFunction(boxRTFunction((void*)strJoin, STR, 2)));

    str_cls->giveAttr("replace",
                      new BoxedFunction(boxRTFunction((void*)strReplace, UNKNOWN, 4, 1, false, false), { boxInt(-1) }));

    for (auto& md : string_methods) {
        str_cls->giveAttr(md.ml_name, new BoxedMethodDescriptor(&md, str_cls));
    }

    CLFunction* count = boxRTFunction((void*)strCount2Unboxed, INT, 2);
    addRTFunction(count, (void*)strCount2, BOXED_INT);
    str_cls->giveAttr("count", new BoxedFunction(count));
    str_cls->giveAttr("index", new BoxedFunction(boxRTFunction((void*)strIndex, BOXED_INT, 2)));

    str_cls->giveAttr("__new__", new BoxedFunction(boxRTFunction((void*)strNew, UNKNOWN, 2, 1, false, false),
                                                   { boxStrConstant("") }));

    str_cls->freeze();

    basestring_cls->giveAttr(
        "__doc__", boxStrConstant("Type basestring cannot be instantiated; it is the base for str and unicode."));
    basestring_cls->giveAttr("__new__",
                             new BoxedFunction(boxRTFunction((void*)basestringNew, UNKNOWN, 1, 0, true, true)));
    basestring_cls->freeze();
}

void teardownStr() {
}
}
