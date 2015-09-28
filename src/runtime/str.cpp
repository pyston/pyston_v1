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

#include "capi/typeobject.h"
#include "capi/types.h"
#include "core/common.h"
#include "core/types.h"
#include "core/util.h"
#include "runtime/dict.h"
#include "runtime/long.h"
#include "runtime/objmodel.h"
#include "runtime/types.h"
#include "runtime/util.h"

extern "C" PyObject* string_count(PyStringObject* self, PyObject* args) noexcept;
extern "C" PyObject* string_join(PyStringObject* self, PyObject* orig) noexcept;
extern "C" PyObject* string_split(PyStringObject* self, PyObject* args) noexcept;
extern "C" PyObject* string_rsplit(PyStringObject* self, PyObject* args) noexcept;
extern "C" PyObject* string_find(PyStringObject* self, PyObject* args) noexcept;
extern "C" PyObject* string_index(PyStringObject* self, PyObject* args) noexcept;
extern "C" PyObject* string_rindex(PyStringObject* self, PyObject* args) noexcept;
extern "C" PyObject* string_rfind(PyStringObject* self, PyObject* args) noexcept;
extern "C" PyObject* string_splitlines(PyStringObject* self, PyObject* args) noexcept;
extern "C" PyObject* string__format__(PyObject* self, PyObject* args) noexcept;

// from cpython's stringobject.c:
#define LEFTSTRIP 0
#define RIGHTSTRIP 1
#define BOTHSTRIP 2

namespace pyston {

BoxedString* EmptyString;
BoxedString* characters[UCHAR_MAX + 1];

BoxedString::BoxedString(const char* s, size_t n) : interned_state(SSTATE_NOT_INTERNED) {
    assert(s);
    RELEASE_ASSERT(n != llvm::StringRef::npos, "");
    memmove(data(), s, n);
    data()[n] = 0;
}

BoxedString::BoxedString(llvm::StringRef lhs, llvm::StringRef rhs) : interned_state(SSTATE_NOT_INTERNED) {
    RELEASE_ASSERT(lhs.size() + rhs.size() != llvm::StringRef::npos, "");
    memmove(data(), lhs.data(), lhs.size());
    memmove(data() + lhs.size(), rhs.data(), rhs.size());
    data()[lhs.size() + rhs.size()] = 0;
}

BoxedString::BoxedString(llvm::StringRef s) : interned_state(SSTATE_NOT_INTERNED) {
    RELEASE_ASSERT(s.size() != llvm::StringRef::npos, "");
    memmove(data(), s.data(), s.size());
    data()[s.size()] = 0;
}

BoxedString::BoxedString(size_t n, char c) : interned_state(SSTATE_NOT_INTERNED) {
    RELEASE_ASSERT(n != llvm::StringRef::npos, "");
    memset(data(), c, n);
    data()[n] = 0;
}

BoxedString::BoxedString(size_t n) : interned_state(SSTATE_NOT_INTERNED) {
    RELEASE_ASSERT(n != llvm::StringRef::npos, "");
    // Note: no memset.  add the null-terminator for good measure though
    // (CPython does the same thing).
    data()[n] = 0;
}

extern "C" char PyString_GetItem(PyObject* op, ssize_t n) noexcept {
    RELEASE_ASSERT(PyString_Check(op), "");
    return static_cast<const BoxedString*>(op)->s()[n];
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
    assert(PyString_Check(lhs));

    if (isSubclass(_rhs->cls, unicode_cls)) {
        Box* rtn = PyUnicode_Concat(lhs, _rhs);
        checkAndThrowCAPIException();
        return rtn;
    }

    if (!PyString_Check(_rhs)) {
        if (PyByteArray_Check(_rhs)) {
            Box* rtn = PyByteArray_Concat(lhs, _rhs);
            checkAndThrowCAPIException();
            return rtn;
        } else {
            // This is a compatibility break with CPython, which has their sq_concat method
            // directly throw a TypeError.  Since we're not implementing this as a sq_concat,
            // return NotImplemented for now.
            return NotImplemented;
        }
    }

    BoxedString* rhs = static_cast<BoxedString*>(_rhs);
    return new (lhs->size() + rhs->size()) BoxedString(lhs->s(), rhs->s());
}

static llvm::StringMap<BoxedString*> interned_strings;
static StatCounter num_interned_strings("num_interned_string");
extern "C" PyObject* PyString_InternFromString(const char* s) noexcept {
    RELEASE_ASSERT(s, "");
    return internStringImmortal(s);
}

BoxedString* internStringImmortal(llvm::StringRef s) {
    auto& entry = interned_strings[s];
    if (!entry) {
        num_interned_strings.log();
        entry = (BoxedString*)PyGC_AddRoot(boxString(s));
        // CPython returns mortal but in our current implementation they are inmortal
        entry->interned_state = SSTATE_INTERNED_IMMORTAL;
    }
    return entry;
}

extern "C" void PyString_InternInPlace(PyObject** p) noexcept {
    BoxedString* s = (BoxedString*)*p;
    if (s == NULL || !PyString_Check(s))
        Py_FatalError("PyString_InternInPlace: strings only please!");
    /* If it's a string subclass, we don't really know what putting
       it in the interned dict might do. */
    if (!PyString_CheckExact(s))
        return;

    if (PyString_CHECK_INTERNED(s))
        return;

    auto& entry = interned_strings[s->s()];
    if (entry)
        *p = entry;
    else {
        num_interned_strings.log();
        entry = (BoxedString*)PyGC_AddRoot(s);

        // CPython returns mortal but in our current implementation they are inmortal
        s->interned_state = SSTATE_INTERNED_IMMORTAL;
    }
}

extern "C" int _PyString_CheckInterned(PyObject* p) noexcept {
    RELEASE_ASSERT(PyString_Check(p), "");
    BoxedString* s = (BoxedString*)p;
    return s->interned_state;
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
            result = Py_TYPE(val)->tp_str(val);
            break;
        case 'o':
            result = Py_TYPE(val)->tp_as_number->nb_oct(val);
            break;
        case 'x':
        case 'X':
            numnondigits = 2;
            result = Py_TYPE(val)->tp_as_number->nb_hex(val);
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
    assert(PyString_Check(lhs));

    int n;

    if (PyIndex_Check(rhs)) {
        Box* index = PyNumber_Index(rhs);
        if (!index) {
            throwCAPIException();
        }
        rhs = index;
    }

    if (PyInt_Check(rhs))
        n = static_cast<BoxedInt*>(rhs)->n;
    else if (isSubclass(rhs->cls, long_cls)) {
        n = _PyLong_AsInt(rhs);
        if (PyErr_Occurred()) {
            PyErr_Clear();
            raiseExcHelper(OverflowError, "cannot fit 'long' into index-sized integer");
        }
    } else
        return NotImplemented;
    if (n <= 0)
        return EmptyString;

    // TODO: use createUninitializedString and getWriteableStringContents
    int sz = lhs->size();
    std::string buf(sz * n, '\0');
    for (int i = 0; i < n; i++) {
        memcpy(&buf[sz * i], lhs->data(), sz);
    }
    return boxString(buf);
}

Box* str_richcompare(Box* lhs, Box* rhs, int op) {
    assert(PyString_Check(lhs));

    // Note: it is somehow about 50% faster to do this check inside the switch
    // statement, rather than out here.  It's functionally equivalent but the
    // generated assembly is somehow quite better:
    // if (unlikely(!PyString_Check(rhs)))
    // return NotImplemented;

    BoxedString* slhs = static_cast<BoxedString*>(lhs);
    BoxedString* srhs = static_cast<BoxedString*>(rhs);

    switch (op) {
        case Py_EQ:
            if (unlikely(!PyString_Check(rhs)))
                return NotImplemented;
            return boxBool(slhs->s() == srhs->s());
        case Py_NE:
            if (unlikely(!PyString_Check(rhs)))
                return NotImplemented;
            return boxBool(slhs->s() != srhs->s());
        case Py_LT:
            if (unlikely(!PyString_Check(rhs)))
                return NotImplemented;
            return boxBool(slhs->s() < srhs->s());
        case Py_LE:
            if (unlikely(!PyString_Check(rhs)))
                return NotImplemented;
            return boxBool(slhs->s() <= srhs->s());
        case Py_GT:
            if (unlikely(!PyString_Check(rhs)))
                return NotImplemented;
            return boxBool(slhs->s() > srhs->s());
        case Py_GE:
            if (unlikely(!PyString_Check(rhs)))
                return NotImplemented;
            return boxBool(slhs->s() >= srhs->s());
        default:
            llvm_unreachable("invalid op");
    }
}

#define JUST_LEFT 0
#define JUST_RIGHT 1
#define JUST_CENTER 2
static Box* pad(BoxedString* self, Box* width, Box* fillchar, int justType) {
    assert(width->cls == int_cls);
    assert(PyString_Check(fillchar));
    assert(static_cast<BoxedString*>(fillchar)->size() == 1);
    int64_t curWidth = self->size();
    int64_t targetWidth = static_cast<BoxedInt*>(width)->n;

    if (curWidth >= targetWidth) {
        if (self->cls == str_cls) {
            return self;
        } else {
            // If self isn't a string but a subclass of str, then make a new string to return
            return boxString(self->s());
        }
    }

    char c = static_cast<BoxedString*>(fillchar)->s()[0];

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
    return boxStringTwine(llvm::Twine(std::string(padLeft, c)) + self->s() + std::string(padRight, c));
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
    assert(PyString_Check(self));

    return boxInt(self->size());
}

extern "C" Box* strStr(BoxedString* self) {
    assert(PyString_Check(self));

    if (self->cls == str_cls)
        return self;

    return boxString(self->s());
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
    BoxedString* op = (BoxedString*)obj;
    size_t newsize = 2 + 4 * Py_SIZE(op);
    PyObject* v;
    if (newsize > PY_SSIZE_T_MAX || newsize / 4 != Py_SIZE(op)) {
        PyErr_SetString(PyExc_OverflowError, "string is too large to make repr");
        return NULL;
    }
    v = PyString_FromStringAndSize((char*)NULL, newsize);
    if (v == NULL) {
        return NULL;
    } else {
        Py_ssize_t i;
        char c;
        char* p;
        int quote;

        /* figure out which quote to use; single is preferred */
        quote = '\'';
        if (smartquotes && memchr(op->data(), '\'', Py_SIZE(op)) && !memchr(op->data(), '"', Py_SIZE(op)))
            quote = '"';

        p = PyString_AS_STRING(v);
        *p++ = quote;
        for (i = 0; i < Py_SIZE(op); i++) {
            /* There's at least enough room for a hex escape
             *                and a closing quote. */
            assert(newsize - (p - PyString_AS_STRING(v)) >= 5);
            c = op->data()[i];
            if (c == quote || c == '\\')
                *p++ = '\\', *p++ = c;
            else if (c == '\t')
                *p++ = '\\', *p++ = 't';
            else if (c == '\n')
                *p++ = '\\', *p++ = 'n';
            else if (c == '\r')
                *p++ = '\\', *p++ = 'r';
            else if (c < ' ' || c >= 0x7f) {
                /* For performance, we don't want to call
                 *                    PyOS_snprintf here (extra layers of
                 *                                       function call). */
                sprintf(p, "\\x%02x", c & 0xff);
                p += 4;
            } else
                *p++ = c;
        }
        assert(newsize - (p - PyString_AS_STRING(v)) >= 1);
        *p++ = quote;
        *p = '\0';
        if (_PyString_Resize(&v, (p - PyString_AS_STRING(v))))
            return NULL;
        return v;
    }
}

extern "C" Box* strRepr(BoxedString* self) {
    return PyString_Repr(self, 1 /* smartquotes */);
}

extern "C" Box* str_repr(Box* self) noexcept {
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

extern "C" size_t unicodeHashUnboxed(PyUnicodeObject* self) {
    Py_ssize_t len;
    Py_UNICODE* p;
    long x;

#ifdef Py_DEBUG
    assert(_Py_HashSecret_Initialized);
#endif
    if (self->hash != -1)
        return self->hash;
    len = PyUnicode_GET_SIZE(self);
    /*
      We make the hash of the empty string be 0, rather than using
      (prefix ^ suffix), since this slightly obfuscates the hash secret
    */
    if (len == 0) {
        self->hash = 0;
        return 0;
    }
    p = PyUnicode_AS_UNICODE(self);
    x = _Py_HashSecret.prefix;
    x ^= *p << 7;
    while (--len >= 0)
        x = (1000003 * x) ^ *p++;
    x ^= PyUnicode_GET_SIZE(self);
    x ^= _Py_HashSecret.suffix;
    if (x == -1)
        x = -2;
    self->hash = x;
    return x;
}

extern "C" size_t strHashUnboxed(BoxedString* self) {
    assert(PyString_Check(self));
    const char* p;
    long x;

#ifdef Py_DEBUG
    assert(_Py_HashSecret_Initialized);
#endif

    long len = Py_SIZE(self);
    /*
      We make the hash of the empty string be 0, rather than using
      (prefix ^ suffix), since this slightly obfuscates the hash secret
    */
    if (len == 0) {
        return 0;
    }
    p = self->s().data();
    x = _Py_HashSecret.prefix;
    x ^= *p << 7;
    while (--len >= 0)
        x = (1000003 * x) ^ *p++;
    x ^= Py_SIZE(self);
    x ^= _Py_HashSecret.suffix;
    if (x == -1)
        x = -2;

    return x;
}

extern "C" Box* strHash(BoxedString* self) {
    return boxInt(strHashUnboxed(self));
}

size_t str_hash(BoxedString* self) noexcept {
    return strHashUnboxed(self);
}

extern "C" Box* strNonzero(BoxedString* self) {
    ASSERT(PyString_Check(self), "%s", self->cls->tp_name);

    return boxBool(self->size() != 0);
}

extern "C" Box* strNew(BoxedClass* cls, Box* obj) {
    assert(isSubclass(cls, str_cls));

    if (cls != str_cls) {
        Box* tmp = strNew(str_cls, obj);
        assert(PyString_Check(tmp));
        BoxedString* tmp_s = static_cast<BoxedString*>(tmp);

        return new (cls, tmp_s->size()) BoxedString(tmp_s->s());
    }

    Box* r = PyObject_Str(obj);
    if (!r)
        throwCAPIException();
    assert(PyString_Check(r));
    return r;
}

extern "C" Box* basestringNew(BoxedClass* cls, Box* args, Box* kwargs) {
    raiseExcHelper(TypeError, "The basestring type cannot be instantiated");
}

Box* _strSlice(BoxedString* self, i64 start, i64 stop, i64 step, i64 length) {
    assert(PyString_Check(self));

    llvm::StringRef s = self->s();

    assert(step != 0);
    if (step > 0) {
        assert(0 <= start);
        assert(stop <= (i64)s.size());
    } else {
        assert(start < (i64)s.size());
        assert(-1 <= stop);
    }
    assert(length >= 0);

    if (length == 0)
        return EmptyString;

    BoxedString* bs = BoxedString::createUninitializedString(length);
    copySlice(bs->data(), s.data(), start, step, length);
    return bs;
}

static Box* str_slice(Box* o, Py_ssize_t i, Py_ssize_t j) {
    BoxedString* a = static_cast<BoxedString*>(o);
    if (i < 0)
        i = 0;
    if (j < 0)
        j = 0; /* Avoid signed/unsigned bug in next line */
    if (j > Py_SIZE(a))
        j = Py_SIZE(a);
    if (i == 0 && j == Py_SIZE(a) && PyString_CheckExact(a)) {
        /* It's the same as a */
        Py_INCREF(a);
        return (PyObject*)a;
    }
    if (j < i)
        j = i;
    return PyString_FromStringAndSize(a->data() + i, j - i);
}

// Analoguous to CPython's, used for sq_ slots.
static Py_ssize_t str_length(Box* a) {
    return Py_SIZE(a);
}

Box* strIsAlpha(BoxedString* self) {
    assert(PyString_Check(self));

    llvm::StringRef str(self->s());
    if (str.empty())
        return False;

    for (const auto& c : str) {
        if (!std::isalpha(c))
            return False;
    }

    return True;
}

Box* strIsDigit(BoxedString* self) {
    assert(PyString_Check(self));

    llvm::StringRef str(self->s());
    if (str.empty())
        return False;

    for (const auto& c : str) {
        if (!std::isdigit(c))
            return False;
    }

    return True;
}

Box* strIsAlnum(BoxedString* self) {
    assert(PyString_Check(self));

    llvm::StringRef str(self->s());
    if (str.empty())
        return False;

    for (const auto& c : str) {
        if (!std::isalnum(c))
            return False;
    }

    return True;
}

Box* strIsLower(BoxedString* self) {
    assert(PyString_Check(self));

    llvm::StringRef str(self->s());
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
    assert(PyString_Check(self));

    llvm::StringRef str(self->s());

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
    assert(PyString_Check(self));

    llvm::StringRef str(self->s());
    if (str.empty())
        return False;

    for (const auto& c : str) {
        if (!std::isspace(c))
            return False;
    }

    return True;
}

Box* strIsTitle(BoxedString* self) {
    assert(PyString_Check(self));

    llvm::StringRef str(self->s());

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

extern "C" PyObject* _PyString_Join(PyObject* sep, PyObject* x) noexcept {
    RELEASE_ASSERT(PyString_Check(sep), "");
    return string_join((PyStringObject*)sep, x);
}

Box* strReplace(Box* _self, Box* _old, Box* _new, Box** _args) {
    if (!PyString_Check(_self))
        raiseExcHelper(TypeError, "descriptor 'replace' requires a 'str' object but received a '%s'",
                       getTypeName(_self));
    BoxedString* self = static_cast<BoxedString*>(_self);

#ifdef Py_USING_UNICODE
    if (PyUnicode_Check(_old) || PyUnicode_Check(_new))
        return PyUnicode_Replace((PyObject*)self, _old, _new, -1 /*count*/);
#endif

    if (!PyString_Check(_old))
        raiseExcHelper(TypeError, "expected a character buffer object");
    BoxedString* old = static_cast<BoxedString*>(_old);

    if (!PyString_Check(_new))
        raiseExcHelper(TypeError, "expected a character buffer object");
    BoxedString* new_ = static_cast<BoxedString*>(_new);

    Box* _maxreplace = _args[0];
    if (!PyInt_Check(_maxreplace))
        raiseExcHelper(TypeError, "an integer is required");

    int max_replaces = static_cast<BoxedInt*>(_maxreplace)->n;
    size_t start_pos = 0;
    std::string s = self->s();

    bool single_char = old->size() == 1;
    for (int num_replaced = 0; num_replaced < max_replaces || max_replaces < 0; ++num_replaced) {
        if (single_char)
            start_pos = s.find(old->s()[0], start_pos);
        else
            start_pos = s.find(old->s(), start_pos);

        if (start_pos == std::string::npos)
            break;
        s.replace(start_pos, old->size(), new_->s());
        start_pos += new_->size(); // Handles case where 'to' is a substring of 'from'
    }
    return boxString(s);
}

Box* strPartition(BoxedString* self, BoxedString* sep) {
    RELEASE_ASSERT(PyString_Check(self), "");
    RELEASE_ASSERT(PyString_Check(sep), "");

    size_t found_idx;
    if (sep->size() == 1)
        found_idx = self->s().find(sep->s()[0]);
    else
        found_idx = self->s().find(sep->s());
    if (found_idx == std::string::npos)
        return BoxedTuple::create({ self, EmptyString, EmptyString });


    return BoxedTuple::create(
        { boxString(llvm::StringRef(self->data(), found_idx)),
          boxString(llvm::StringRef(self->data() + found_idx, sep->size())),
          boxString(llvm::StringRef(self->data() + found_idx + sep->size(), self->size() - found_idx - sep->size())) });
}

Box* strRpartition(BoxedString* self, BoxedString* sep) {
    RELEASE_ASSERT(PyString_Check(self), "");
    RELEASE_ASSERT(PyString_Check(sep), "");

    size_t found_idx = self->s().rfind(sep->s());
    if (found_idx == std::string::npos)
        return BoxedTuple::create({ EmptyString, EmptyString, self });

    return BoxedTuple::create(
        { boxString(llvm::StringRef(self->data(), found_idx)),
          boxString(llvm::StringRef(self->data() + found_idx, sep->size())),
          boxString(llvm::StringRef(self->data() + found_idx + sep->size(), self->size() - found_idx - sep->size())) });
}

extern "C" PyObject* _do_string_format(PyObject* self, PyObject* args, PyObject* kwargs);

Box* strFormat(BoxedString* self, BoxedTuple* args, BoxedDict* kwargs) {
    assert(args->cls == tuple_cls);
    assert(!kwargs || kwargs->cls == dict_cls);

    Box* rtn = _do_string_format(self, args, kwargs);
    checkAndThrowCAPIException();
    assert(rtn);
    return rtn;
}

Box* strStrip(BoxedString* self, Box* chars) {
    assert(PyString_Check(self));
    auto str = self->s();

    if (PyString_Check(chars)) {
        auto chars_str = static_cast<BoxedString*>(chars)->s();
        return boxString(str.trim(chars_str));
    } else if (chars->cls == none_cls) {
        return boxString(str.trim(" \t\n\r\f\v"));
    } else if (isSubclass(chars->cls, unicode_cls)) {
        PyObject* uniself = PyUnicode_FromObject((PyObject*)self);
        PyObject* res;
        if (uniself == NULL)
            throwCAPIException();
        res = _PyUnicode_XStrip((PyUnicodeObject*)uniself, BOTHSTRIP, chars);
        if (!res)
            throwCAPIException();
        Py_DECREF(uniself);
        return res;
    } else {
        raiseExcHelper(TypeError, "strip arg must be None, str or unicode");
    }
}

Box* strLStrip(BoxedString* self, Box* chars) {
    assert(PyString_Check(self));
    auto str = self->s();

    if (PyString_Check(chars)) {
        auto chars_str = static_cast<BoxedString*>(chars)->s();
        return boxString(str.ltrim(chars_str));
    } else if (chars->cls == none_cls) {
        return boxString(str.ltrim(" \t\n\r\f\v"));
    } else if (isSubclass(chars->cls, unicode_cls)) {
        PyObject* uniself = PyUnicode_FromObject((PyObject*)self);
        PyObject* res;
        if (uniself == NULL)
            throwCAPIException();
        res = _PyUnicode_XStrip((PyUnicodeObject*)uniself, LEFTSTRIP, chars);
        if (!res)
            throwCAPIException();
        Py_DECREF(uniself);
        return res;
    } else {
        raiseExcHelper(TypeError, "lstrip arg must be None, str or unicode");
    }
}

Box* strRStrip(BoxedString* self, Box* chars) {
    assert(PyString_Check(self));
    auto str = self->s();

    if (PyString_Check(chars)) {
        auto chars_str = static_cast<BoxedString*>(chars)->s();
        return boxString(str.rtrim(chars_str));
    } else if (chars->cls == none_cls) {
        return boxString(str.rtrim(" \t\n\r\f\v"));
    } else if (isSubclass(chars->cls, unicode_cls)) {
        PyObject* uniself = PyUnicode_FromObject((PyObject*)self);
        PyObject* res;
        if (uniself == NULL)
            throwCAPIException();
        res = _PyUnicode_XStrip((PyUnicodeObject*)uniself, RIGHTSTRIP, chars);
        if (!res)
            throwCAPIException();
        Py_DECREF(uniself);
        return res;
    } else {
        raiseExcHelper(TypeError, "rstrip arg must be None, str or unicode");
    }
}

Box* strCapitalize(BoxedString* self) {
    assert(PyString_Check(self));

    std::string s(self->s());

    for (auto& i : s) {
        i = std::tolower(i);
    }

    if (!s.empty()) {
        s[0] = std::toupper(s[0]);
    }

    return boxString(s);
}

Box* strTitle(BoxedString* self) {
    assert(PyString_Check(self));

    std::string s(self->s());
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
    if (!PyString_Check(self))
        raiseExcHelper(TypeError, "descriptor 'translate' requires a 'str' object but received a '%s'",
                       getTypeName(self));

    std::unordered_set<char> delete_set;
    if (delete_chars) {
        if (!PyString_Check(delete_chars))
            raiseExcHelper(TypeError, "expected a character buffer object");
        delete_set.insert(delete_chars->s().begin(), delete_chars->s().end());
    }

    bool have_table = table != None;
    if (have_table) {
        if (!PyString_Check(table))
            raiseExcHelper(TypeError, "expected a character buffer object");
        if (table->size() != 256)
            raiseExcHelper(ValueError, "translation table must be 256 characters long");
    }

    std::string str;
    for (const char c : self->s()) {
        if (!delete_set.count(c))
            str.append(1, have_table ? table->s()[(unsigned char)c] : c);
    }
    return boxString(str);
}

Box* strLower(BoxedString* self) {
    assert(PyString_Check(self));

    BoxedString* rtn = new (self->size()) BoxedString(self->s());
    for (int i = 0; i < rtn->size(); i++)
        rtn->data()[i] = std::tolower(rtn->data()[i]);
    return rtn;
}

Box* strUpper(BoxedString* self) {
    assert(PyString_Check(self));
    BoxedString* rtn = new (self->size()) BoxedString(self->s());
    for (int i = 0; i < rtn->size(); i++)
        rtn->data()[i] = std::toupper(rtn->data()[i]);
    return rtn;
}

Box* strSwapcase(BoxedString* self) {
    assert(PyString_Check(self));
    BoxedString* rtn = new (self->size()) BoxedString(self->s());
    for (int i = 0; i < rtn->size(); i++) {
        char c = rtn->data()[i];
        if (std::islower(c))
            rtn->data()[i] = std::toupper(c);
        else if (std::isupper(c))
            rtn->data()[i] = std::tolower(c);
    }
    return rtn;
}

static inline int string_contains_shared(BoxedString* self, Box* elt) {
    assert(PyString_Check(self));

    if (PyUnicode_Check(elt)) {
        int r = PyUnicode_Contains(self, elt);
        if (r < 0)
            throwCAPIException();
        return r;
    }

    if (!PyString_Check(elt))
        raiseExcHelper(TypeError, "'in <string>' requires string as left operand, not %s", getTypeName(elt));

    BoxedString* sub = static_cast<BoxedString*>(elt);

    size_t found_idx;
    if (sub->size() == 1)
        // Call the much-faster single-character find():
        found_idx = self->s().find(sub->s()[0]);
    else
        found_idx = self->s().find(sub->s());
    return (found_idx != std::string::npos);
}

// Analoguous to CPython's, used for sq_ slots.
static int string_contains(PyObject* str_obj, PyObject* sub_obj) {
    return string_contains_shared((BoxedString*)str_obj, sub_obj);
}

Box* strContains(BoxedString* self, Box* elt) {
    return boxBool(string_contains_shared(self, elt));
}

// compares (a+a_pos, len) with (str)
// if len == npos, compare to the end of a
static int compareStringRefs(llvm::StringRef a, size_t a_pos, size_t len, llvm::StringRef str) {
    if (len == llvm::StringRef::npos)
        len = a.size() - a_pos;
    if (a_pos + len > a.size())
        throw std::out_of_range("pos+len out of range");
    return llvm::StringRef(a.data() + a_pos, len).compare(str);
}

extern "C" int _PyString_Eq(PyObject* o1, PyObject* o2) noexcept {
    assert(PyString_Check(o1));
    assert(PyString_Check(o2));
    BoxedString* a = (BoxedString*)o1;
    BoxedString* b = (BoxedString*)o2;
    return a->s() == b->s();
}

Box* strStartswith(BoxedString* self, Box* elt, Box* start, Box** _args) {
    Box* end = _args[0];

    if (!PyString_Check(self))
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

    if (PyTuple_Check(elt)) {
        for (auto e : *static_cast<BoxedTuple*>(elt)) {
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

    if (!PyString_Check(elt))
        raiseExcHelper(TypeError, "expected a character buffer object");

    BoxedString* sub = static_cast<BoxedString*>(elt);

    Py_ssize_t n = self->size();
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
    if (sub->size() > compare_len)
        return False;
    return boxBool(compareStringRefs(self->s(), istart, sub->size(), sub->s()) == 0);
}

Box* strEndswith(BoxedString* self, Box* elt, Box* start, Box** _args) {
    Box* end = _args[0];

    if (!PyString_Check(self))
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

    if (PyTuple_Check(elt)) {
        for (auto e : *static_cast<BoxedTuple*>(elt)) {
            auto b = strEndswith(self, e, start, _args);
            assert(b->cls == bool_cls);
            if (b == True)
                return True;
        }
        return False;
    }

    if (!PyString_Check(elt))
        raiseExcHelper(TypeError, "expected a character buffer object");

    BoxedString* sub = static_cast<BoxedString*>(elt);

    Py_ssize_t n = self->size();
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
    if (sub->size() > compare_len)
        return False;
    // XXX: this line is the only difference between startswith and endswith:
    istart += compare_len - sub->size();
    return boxBool(compareStringRefs(self->s(), istart, sub->size(), sub->s()) == 0);
}

Box* strDecode(BoxedString* self, Box* encoding, Box* error) {
    if (!PyString_Check(self))
        raiseExcHelper(TypeError, "descriptor 'decode' requires a 'str' object but received a '%s'", getTypeName(self));

    BoxedString* encoding_str = (BoxedString*)encoding;
    BoxedString* error_str = (BoxedString*)error;

    if (encoding_str && encoding_str->cls == unicode_cls)
        encoding_str = (BoxedString*)_PyUnicode_AsDefaultEncodedString(encoding_str, NULL);

    if (encoding_str && !PyString_Check(encoding_str))
        raiseExcHelper(TypeError, "decode() argument 1 must be string, not '%s'", getTypeName(encoding_str));

    if (error_str && error_str->cls == unicode_cls)
        error_str = (BoxedString*)_PyUnicode_AsDefaultEncodedString(error_str, NULL);

    if (error_str && !PyString_Check(error_str))
        raiseExcHelper(TypeError, "decode() argument 2 must be string, not '%s'", getTypeName(error_str));

    Box* result = PyString_AsDecodedObject(self, encoding_str ? encoding_str->data() : NULL,
                                           error_str ? error_str->data() : NULL);
    checkAndThrowCAPIException();
    return result;
}

Box* strEncode(BoxedString* self, Box* encoding, Box* error) {
    if (!PyString_Check(self))
        raiseExcHelper(TypeError, "descriptor 'encode' requires a 'str' object but received a '%s'", getTypeName(self));

    BoxedString* encoding_str = (BoxedString*)encoding;
    BoxedString* error_str = (BoxedString*)error;

    if (encoding_str && encoding_str->cls == unicode_cls)
        encoding_str = (BoxedString*)_PyUnicode_AsDefaultEncodedString(encoding_str, NULL);

    if (encoding_str && !PyString_Check(encoding_str))
        raiseExcHelper(TypeError, "encode() argument 1 must be string, not '%s'", getTypeName(encoding_str));

    if (error_str && error_str->cls == unicode_cls)
        error_str = (BoxedString*)_PyUnicode_AsDefaultEncodedString(error_str, NULL);

    if (error_str && !PyString_Check(error_str))
        raiseExcHelper(TypeError, "encode() argument 2 must be string, not '%s'", getTypeName(error_str));

    Box* result = PyString_AsEncodedObject(self, encoding_str ? encoding_str->data() : PyUnicode_GetDefaultEncoding(),
                                           error_str ? error_str->data() : NULL);
    checkAndThrowCAPIException();
    return result;
}

static PyObject* string_item(PyStringObject* self, register Py_ssize_t i) {
    BoxedString* boxedString = (BoxedString*)self;

    if (i < 0 || i >= boxedString->size()) {
        raiseExcHelper(IndexError, "string index out of range");
    }

    char c = boxedString->s()[i];
    return characters[c & UCHAR_MAX];
}

template <ExceptionStyle S> Box* strGetitem(BoxedString* self, Box* slice) {
    if (S == CAPI) {
        try {
            return strGetitem<CXX>(self, slice);
        } catch (ExcInfo e) {
            setCAPIException(e);
            return NULL;
        }
    }
    assert(PyString_Check(self));

    if (PyIndex_Check(slice)) {
        Py_ssize_t n = PyNumber_AsSsize_t(slice, PyExc_IndexError);
        if (n == -1 && PyErr_Occurred())
            throwCAPIException();
        int size = self->size();
        if (n < 0)
            n = size + n;

        if (n < 0 || n >= size) {
            raiseExcHelper(IndexError, "string index out of range");
        }

        char c = self->s()[n];
        return characters[c & UCHAR_MAX];
    } else if (slice->cls == slice_cls) {
        BoxedSlice* sslice = static_cast<BoxedSlice*>(slice);

        i64 start, stop, step, length;
        parseSlice(sslice, self->size(), &start, &stop, &step, &length);
        return _strSlice(self, start, stop, step, length);
    } else {
        raiseExcHelper(TypeError, "string indices must be integers, not %s", getTypeName(slice));
    }
}

extern "C" Box* strGetslice(BoxedString* self, Box* boxedStart, Box* boxedStop) {
    assert(PyString_Check(self));

    i64 start, stop;
    sliceIndex(boxedStart, &start);
    sliceIndex(boxedStop, &stop);

    boundSliceWithLength(&start, &stop, start, stop, self->s().size());

    return _strSlice(self, start, stop, 1, stop - start);
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

    BoxedStringIterator(BoxedString* s) : s(s), it(s->s().begin()), end(s->s().end()) {}

    DEFAULT_CLASS(str_iterator_cls);

    static bool hasnextUnboxed(BoxedStringIterator* self) {
        assert(self->cls == str_iterator_cls);
        return self->it != self->end;
    }

    static Box* hasnext(BoxedStringIterator* self) {
        assert(self->cls == str_iterator_cls);
        return boxBool(self->it != self->end);
    }

    static Box* iter(BoxedStringIterator* self) {
        assert(self->cls == str_iterator_cls);
        return self;
    }

    static Box* next(BoxedStringIterator* self) {
        assert(self->cls == str_iterator_cls);
        if (!hasnextUnboxed(self))
            raiseExcHelper(StopIteration, (const char*)nullptr);

        char c = *self->it;
        ++self->it;
        return characters[c & UCHAR_MAX];
    }

    static Box* next_capi(Box* _self) noexcept {
        assert(_self->cls == str_iterator_cls);
        auto self = (BoxedStringIterator*)_self;
        if (!hasnextUnboxed(self))
            return NULL;

        char c = *self->it;
        ++self->it;
        return characters[c & UCHAR_MAX];
    }

    static void gcHandler(GCVisitor* v, Box* b) {
        Box::gcHandler(v, b);
        BoxedStringIterator* it = (BoxedStringIterator*)b;
        v->visit(&it->s);
    }
};

Box* strIter(BoxedString* self) noexcept {
    assert(PyString_Check(self));
    return new BoxedStringIterator(self);
}

extern "C" PyObject* PyString_FromString(const char* s) noexcept {
    return boxString(s);
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

extern "C" PyObject* PyString_FromStringAndSize(const char* s, ssize_t n) noexcept {
    if (s == NULL)
        return BoxedString::createUninitializedString(n);
    return boxString(llvm::StringRef(s, n));
}

static /*const*/ char* string_getbuffer(register PyObject* op) noexcept {
    char* s;
    Py_ssize_t len;
    if (PyString_AsStringAndSize(op, &s, &len))
        return NULL;
    return s;
}

extern "C" char* PyString_AsString(PyObject* o) noexcept {
    if (!PyString_Check(o))
        return string_getbuffer(o);

    BoxedString* s = static_cast<BoxedString*>(o);
    return s->getWriteableStringContents();
}

extern "C" Py_ssize_t PyString_Size(PyObject* op) noexcept {
    if (PyString_Check(op))
        return static_cast<BoxedString*>(op)->size();

    char* _s;
    Py_ssize_t len;
    if (PyString_AsStringAndSize(op, &_s, &len))
        return -1;
    return len;
}

extern "C" int _PyString_Resize(PyObject** pv, Py_ssize_t newsize) noexcept {
    // This is only allowed to be called when there is only one user of the string (ie a refcount of 1 in CPython)

    assert(pv);
    assert(PyString_Check(*pv));
    BoxedString* s = static_cast<BoxedString*>(*pv);

    if (newsize == s->size())
        return 0;

    if (PyString_CHECK_INTERNED(s)) {
        *pv = 0;
        return -1;
    }

    if (newsize < s->size()) {
        // XXX resize the box (by reallocating) smaller if it makes sense
        s->ob_size = newsize;
        s->data()[newsize] = 0;
        return 0;
    }

    BoxedString* resized;

    if (s->cls == str_cls)
        resized = new (newsize) BoxedString(newsize, 0); // we need an uninitialized string, but this will memset
    else
        resized = new (s->cls, newsize)
            BoxedString(newsize, 0); // we need an uninitialized string, but this will memset
    memmove(resized->data(), s->data(), s->size());

    *pv = resized;
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

static PyObject* string_zfill(PyObject* self, PyObject* args) {
    Py_ssize_t fill;
    PyObject* s;
    char* p;
    Py_ssize_t width;

    if (!PyArg_ParseTuple(args, "n:zfill", &width))
        return NULL;

    if (PyString_GET_SIZE(self) >= width) {
        if (PyString_CheckExact(self)) {
            Py_INCREF(self);
            return (PyObject*)self;
        } else
            return PyString_FromStringAndSize(PyString_AS_STRING(self), PyString_GET_SIZE(self));
    }

    fill = width - PyString_GET_SIZE(self);

    // Pyston change:
    // s = pad(self, fill, 0, '0');
    s = pad((BoxedString*)self, boxInt(width), characters['0' & UCHAR_MAX], JUST_RIGHT);

    if (s == NULL)
        return NULL;

    p = PyString_AS_STRING(s);
    if (p[fill] == '+' || p[fill] == '-') {
        /* move sign to beginning of string */
        p[0] = p[fill];
        p[fill] = '0';
    }

    return (PyObject*)s;
}

static int string_print(PyObject* _op, FILE* fp, int flags) noexcept {
    BoxedString* op = (BoxedString*)_op;

    Py_ssize_t i, str_len;
    char c;
    int quote;

    /* XXX Ought to check for interrupts when writing long strings */
    if (!PyString_CheckExact(op)) {
        int ret;
        /* A str subclass may have its own __str__ method. */
        op = (BoxedString*)PyObject_Str((PyObject*)op);
        if (op == NULL)
            return -1;
        ret = string_print(op, fp, flags);
        Py_DECREF(op);
        return ret;
    }
    if (flags & Py_PRINT_RAW) {
        // Pyston change
        // char *data = op->ob_sval;
        // Py_ssize_t size = Py_SIZE(op);
        const char* data = op->data();
        Py_ssize_t size = op->size();
        Py_BEGIN_ALLOW_THREADS while (size > INT_MAX) {
            /* Very long strings cannot be written atomically.
             * But don't write exactly INT_MAX bytes at a time
             * to avoid memory aligment issues.
             */
            const int chunk_size = INT_MAX & ~0x3FFF;
            fwrite(data, 1, chunk_size, fp);
            data += chunk_size;
            size -= chunk_size;
        }
#ifdef __VMS
        if (size)
            fwrite(data, (size_t)size, 1, fp);
#else
        fwrite(data, 1, (size_t)size, fp);
#endif
        Py_END_ALLOW_THREADS return 0;
    }

    /* figure out which quote to use; single is preferred */
    quote = '\'';
    // Pyston change
    // if (memchr(op->ob_sval, '\'', Py_SIZE(op)) && !memchr(op->ob_sval, '"', Py_SIZE(op)))
    if (memchr(op->data(), '\'', Py_SIZE(op)) && !memchr(op->data(), '"', Py_SIZE(op)))
        quote = '"';

    // Pyston change
    // str_len = Py_SIZE(op);
    str_len = op->size();
    Py_BEGIN_ALLOW_THREADS fputc(quote, fp);
    for (i = 0; i < str_len; i++) {
        /* Since strings are immutable and the caller should have a
        reference, accessing the interal buffer should not be an issue
        with the GIL released. */
        // Pyston change:
        // c = op->ob_sval[i];
        c = op->s()[i];
        if (c == quote || c == '\\')
            fprintf(fp, "\\%c", c);
        else if (c == '\t')
            fprintf(fp, "\\t");
        else if (c == '\n')
            fprintf(fp, "\\n");
        else if (c == '\r')
            fprintf(fp, "\\r");
        else if (c < ' ' || c >= 0x7f)
            fprintf(fp, "\\x%02x", c & 0xff);
        else
            fputc(c, fp);
    }
    fputc(quote, fp);
    Py_END_ALLOW_THREADS return 0;
}

static Py_ssize_t string_buffer_getreadbuf(PyObject* self, Py_ssize_t index, const void** ptr) noexcept {
    RELEASE_ASSERT(index == 0, "");
    // I think maybe this can just be a non-release assert?  shouldn't be able to call this with
    // the wrong type
    RELEASE_ASSERT(PyString_Check(self), "");

    auto s = static_cast<BoxedString*>(self);
    *ptr = s->data();
    return s->size();
}

static Py_ssize_t string_buffer_getsegcount(PyObject* o, Py_ssize_t* lenp) noexcept {
    RELEASE_ASSERT(lenp == NULL, "");
    RELEASE_ASSERT(PyString_Check(o), "");

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
    assert(PyString_Check(self));
    return PyBuffer_FillInfo(view, (PyObject*)self, self->data(), self->size(), 1, flags);
}

static PyObject* string_getnewargs(BoxedString* v) noexcept {
    return Py_BuildValue("(s#)", v->data(), v->size());
}

static PyBufferProcs string_as_buffer = {
    (readbufferproc)string_buffer_getreadbuf, // comments are the only way I've found of
    (writebufferproc)NULL,                    // forcing clang-format to break these onto multiple lines
    (segcountproc)string_buffer_getsegcount,  //
    (charbufferproc)string_buffer_getcharbuf, //
    (getbufferproc)string_buffer_getbuffer,   //
    (releasebufferproc)NULL,
};

static PyMethodDef string_methods[] = {
    { "count", (PyCFunction)string_count, METH_O3 | METH_D2, NULL },
    { "join", (PyCFunction)string_join, METH_O, NULL },
    { "split", (PyCFunction)string_split, METH_VARARGS, NULL },
    { "rsplit", (PyCFunction)string_rsplit, METH_VARARGS, NULL },
    { "find", (PyCFunction)string_find, METH_VARARGS, NULL },
    { "index", (PyCFunction)string_index, METH_VARARGS, NULL },
    { "rindex", (PyCFunction)string_rindex, METH_VARARGS, NULL },
    { "rfind", (PyCFunction)string_rfind, METH_VARARGS, NULL },
    { "expandtabs", (PyCFunction)string_expandtabs, METH_VARARGS, NULL },
    { "splitlines", (PyCFunction)string_splitlines, METH_VARARGS, NULL },
    { "zfill", (PyCFunction)string_zfill, METH_VARARGS, NULL },
    { "__format__", (PyCFunction)string__format__, METH_VARARGS, NULL },
};

void setupStr() {
    static PySequenceMethods string_as_sequence;
    str_cls->tp_as_sequence = &string_as_sequence;
    static PyNumberMethods str_as_number;
    str_cls->tp_as_number = &str_as_number;
    static PyMappingMethods str_as_mapping;
    str_cls->tp_as_mapping = &str_as_mapping;

    str_cls->tp_flags |= Py_TPFLAGS_HAVE_NEWBUFFER;

    str_iterator_cls = BoxedClass::create(type_cls, object_cls, &BoxedStringIterator::gcHandler, 0, 0,
                                          sizeof(BoxedStringIterator), false, "striterator");
    str_iterator_cls->giveAttr("__hasnext__",
                               new BoxedFunction(boxRTFunction((void*)BoxedStringIterator::hasnext, BOXED_BOOL, 1)));
    str_iterator_cls->giveAttr("__iter__",
                               new BoxedFunction(boxRTFunction((void*)BoxedStringIterator::iter, UNKNOWN, 1)));
    str_iterator_cls->giveAttr("next", new BoxedFunction(boxRTFunction((void*)BoxedStringIterator::next, STR, 1)));
    str_iterator_cls->freeze();
    str_iterator_cls->tpp_hasnext = (BoxedClass::pyston_inquiry)BoxedStringIterator::hasnextUnboxed;
    str_iterator_cls->tp_iternext = BoxedStringIterator::next_capi;
    str_iterator_cls->tp_iter = PyObject_SelfIter;

    str_cls->tp_as_buffer = &string_as_buffer;
    str_cls->tp_print = string_print;

    str_cls->giveAttr("__getnewargs__", new BoxedFunction(boxRTFunction((void*)string_getnewargs, UNKNOWN, 1,
                                                                        ParamNames::empty(), CAPI)));

    str_cls->giveAttr("__len__", new BoxedFunction(boxRTFunction((void*)strLen, BOXED_INT, 1)));
    str_cls->giveAttr("__str__", new BoxedFunction(boxRTFunction((void*)strStr, STR, 1)));
    str_cls->giveAttr("__repr__", new BoxedFunction(boxRTFunction((void*)strRepr, STR, 1)));
    str_cls->giveAttr("__hash__", new BoxedFunction(boxRTFunction((void*)strHash, UNKNOWN, 1)));
    str_cls->giveAttr("__nonzero__", new BoxedFunction(boxRTFunction((void*)strNonzero, BOXED_BOOL, 1)));

    str_cls->giveAttr("isalnum", new BoxedFunction(boxRTFunction((void*)strIsAlnum, BOXED_BOOL, 1)));
    str_cls->giveAttr("isalpha", new BoxedFunction(boxRTFunction((void*)strIsAlpha, BOXED_BOOL, 1)));
    str_cls->giveAttr("isdigit", new BoxedFunction(boxRTFunction((void*)strIsDigit, BOXED_BOOL, 1)));
    str_cls->giveAttr("islower", new BoxedFunction(boxRTFunction((void*)strIsLower, BOXED_BOOL, 1)));
    str_cls->giveAttr("isspace", new BoxedFunction(boxRTFunction((void*)strIsSpace, BOXED_BOOL, 1)));
    str_cls->giveAttr("istitle", new BoxedFunction(boxRTFunction((void*)strIsTitle, BOXED_BOOL, 1)));
    str_cls->giveAttr("isupper", new BoxedFunction(boxRTFunction((void*)strIsUpper, BOXED_BOOL, 1)));

    str_cls->giveAttr("decode", new BoxedFunction(boxRTFunction((void*)strDecode, UNKNOWN, 3, false, false), { 0, 0 }));
    str_cls->giveAttr("encode", new BoxedFunction(boxRTFunction((void*)strEncode, UNKNOWN, 3, false, false), { 0, 0 }));

    str_cls->giveAttr("lower", new BoxedFunction(boxRTFunction((void*)strLower, STR, 1)));
    str_cls->giveAttr("swapcase", new BoxedFunction(boxRTFunction((void*)strSwapcase, STR, 1)));
    str_cls->giveAttr("upper", new BoxedFunction(boxRTFunction((void*)strUpper, STR, 1)));

    str_cls->giveAttr("strip", new BoxedFunction(boxRTFunction((void*)strStrip, UNKNOWN, 2, false, false), { None }));
    str_cls->giveAttr("lstrip", new BoxedFunction(boxRTFunction((void*)strLStrip, UNKNOWN, 2, false, false), { None }));
    str_cls->giveAttr("rstrip", new BoxedFunction(boxRTFunction((void*)strRStrip, UNKNOWN, 2, false, false), { None }));

    str_cls->giveAttr("capitalize", new BoxedFunction(boxRTFunction((void*)strCapitalize, STR, 1)));
    str_cls->giveAttr("title", new BoxedFunction(boxRTFunction((void*)strTitle, STR, 1)));

    str_cls->giveAttr("translate",
                      new BoxedFunction(boxRTFunction((void*)strTranslate, STR, 3, false, false), { NULL }));

    str_cls->giveAttr("__contains__", new BoxedFunction(boxRTFunction((void*)strContains, BOXED_BOOL, 2)));

    str_cls->giveAttr("startswith",
                      new BoxedFunction(boxRTFunction((void*)strStartswith, BOXED_BOOL, 4, 0, 0), { NULL, NULL }));
    str_cls->giveAttr("endswith",
                      new BoxedFunction(boxRTFunction((void*)strEndswith, BOXED_BOOL, 4, 0, 0), { NULL, NULL }));

    str_cls->giveAttr("partition", new BoxedFunction(boxRTFunction((void*)strPartition, UNKNOWN, 2)));
    str_cls->giveAttr("rpartition", new BoxedFunction(boxRTFunction((void*)strRpartition, UNKNOWN, 2)));

    str_cls->giveAttr("format", new BoxedFunction(boxRTFunction((void*)strFormat, UNKNOWN, 1, true, true)));

    str_cls->giveAttr("__add__", new BoxedFunction(boxRTFunction((void*)strAdd, UNKNOWN, 2)));
    str_cls->giveAttr("__mod__", new BoxedFunction(boxRTFunction((void*)strMod, UNKNOWN, 2)));
    str_cls->giveAttr("__mul__", new BoxedFunction(boxRTFunction((void*)strMul, UNKNOWN, 2)));
    // TODO not sure if this is right in all cases:
    str_cls->giveAttr("__rmul__", new BoxedFunction(boxRTFunction((void*)strMul, UNKNOWN, 2)));

    str_cls->tp_richcompare = str_richcompare;

    BoxedString* spaceChar = characters[' ' & UCHAR_MAX];
    assert(spaceChar);
    str_cls->giveAttr("ljust",
                      new BoxedFunction(boxRTFunction((void*)strLjust, UNKNOWN, 3, false, false), { spaceChar }));
    str_cls->giveAttr("rjust",
                      new BoxedFunction(boxRTFunction((void*)strRjust, UNKNOWN, 3, false, false), { spaceChar }));
    str_cls->giveAttr("center",
                      new BoxedFunction(boxRTFunction((void*)strCenter, UNKNOWN, 3, false, false), { spaceChar }));

    auto str_getitem = boxRTFunction((void*)strGetitem<CXX>, STR, 2, ParamNames::empty(), CXX);
    addRTFunction(str_getitem, (void*)strGetitem<CAPI>, STR, CAPI);
    str_cls->giveAttr("__getitem__", new BoxedFunction(str_getitem));

    str_cls->giveAttr("__getslice__", new BoxedFunction(boxRTFunction((void*)strGetslice, STR, 3)));

    str_cls->giveAttr("__iter__", new BoxedFunction(boxRTFunction((void*)strIter, typeFromClass(str_iterator_cls), 1)));

    str_cls->giveAttr("replace",
                      new BoxedFunction(boxRTFunction((void*)strReplace, UNKNOWN, 4, false, false), { boxInt(-1) }));

    for (auto& md : string_methods) {
        str_cls->giveAttr(md.ml_name, new BoxedMethodDescriptor(&md, str_cls));
    }

    str_cls->giveAttr("__new__",
                      new BoxedFunction(boxRTFunction((void*)strNew, UNKNOWN, 2, false, false), { EmptyString }));

    add_operators(str_cls);
    str_cls->freeze();

    str_cls->tp_repr = str_repr;
    str_cls->tp_iter = (decltype(str_cls->tp_iter))strIter;
    str_cls->tp_hash = (hashfunc)str_hash;
    str_cls->tp_as_sequence->sq_length = str_length;
    str_cls->tp_as_sequence->sq_item = (ssizeargfunc)string_item;
    str_cls->tp_as_sequence->sq_slice = str_slice;
    str_cls->tp_as_sequence->sq_contains = (objobjproc)string_contains;

    basestring_cls->giveAttr("__doc__",
                             boxString("Type basestring cannot be instantiated; it is the base for str and unicode."));
    basestring_cls->giveAttr("__new__", new BoxedFunction(boxRTFunction((void*)basestringNew, UNKNOWN, 1, true, true)));
    basestring_cls->freeze();
}

void teardownStr() {
}
}
