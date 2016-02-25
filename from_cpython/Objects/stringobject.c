// Pyston change: this is just a shim to import stuff from stringlib/

#include "Python.h"

#include "stringlib/stringdefs.h"
#include "stringlib/fastsearch.h"

#include "stringlib/count.h"
#include "stringlib/find.h"
#include "stringlib/split.h"

#define _Py_InsertThousandsGrouping _PyString_InsertThousandsGrouping
#include "stringlib/localeutil.h"

#include "stringlib/string_format.h"

// do_string_format needs to be declared as a static function, since it's used by both stringobject.c
// and unicodeobject.c.  We want to access it from str.cpp, though, so just use this little forwarding
// function.
// We could also potentially have tried to modifie string_format.h to choose whether to mark the function
// as static or not.
PyObject * _do_string_format(PyObject *self, PyObject *args, PyObject *kwargs) {
    return do_string_format(self, args, kwargs);
}

void _string_init() {
    if (PyType_Ready(&PyFieldNameIter_Type) < 0)
        Py_FatalError("Can't initialize field name iterator type");

    if (PyType_Ready(&PyFormatterIter_Type) < 0)
        Py_FatalError("Can't initialize formatter iter type");
}

PyObject * _formatter_parser(STRINGLIB_OBJECT *self) {
    return formatter_parser(self);
}

PyObject * _formatter_field_name_split(STRINGLIB_OBJECT *self) {
    return formatter_field_name_split(self);
}

PyObject *
string_count(PyStringObject *self,
             PyObject *sub_obj, PyObject* obj_start, PyObject** args)
{
    const char *str = PyString_AS_STRING(self), *sub;
    Py_ssize_t sub_len;
    Py_ssize_t start = 0, end = PY_SSIZE_T_MAX;
    PyObject* obj_end = args[0];

    /*
    if (!stringlib_parse_args_finds("count", args, &sub_obj, &start, &end))
        return NULL;
    */

    if (obj_start && obj_start != Py_None)
        if (!_PyEval_SliceIndex(obj_start, &start))
            return 0;
    if (obj_end && obj_end != Py_None)
        if (!_PyEval_SliceIndex(obj_end, &end))
            return 0;

    if (PyString_Check(sub_obj)) {
        sub = PyString_AS_STRING(sub_obj);
        sub_len = PyString_GET_SIZE(sub_obj);
    }
#ifdef Py_USING_UNICODE
    else if (PyUnicode_Check(sub_obj)) {
        Py_ssize_t count;
        count = PyUnicode_Count((PyObject *)self, sub_obj, start, end);
        if (count == -1)
            return NULL;
        else
            return PyInt_FromSsize_t(count);
    }
#endif
    else if (PyObject_AsCharBuffer(sub_obj, &sub, &sub_len))
        return NULL;

    ADJUST_INDICES(start, end, PyString_GET_SIZE(self));

    return PyInt_FromSsize_t(
        stringlib_count(str + start, end - start, sub, sub_len, PY_SSIZE_T_MAX)
        );
}

PyObject * string_split(PyStringObject *self, PyObject *args)
{
    Py_ssize_t len = PyString_GET_SIZE(self), n;
    Py_ssize_t maxsplit = -1;
    const char *s = PyString_AS_STRING(self), *sub;
    PyObject *subobj = Py_None;

    if (!PyArg_ParseTuple(args, "|On:split", &subobj, &maxsplit))
        return NULL;
    if (maxsplit < 0)
        maxsplit = PY_SSIZE_T_MAX;
    if (subobj == Py_None)
        return stringlib_split_whitespace((PyObject*) self, s, len, maxsplit);
    if (PyString_Check(subobj)) {
        sub = PyString_AS_STRING(subobj);
        n = PyString_GET_SIZE(subobj);
    }
#ifdef Py_USING_UNICODE
    else if (PyUnicode_Check(subobj))
        return PyUnicode_Split((PyObject *)self, subobj, maxsplit);
#endif
    else if (PyObject_AsCharBuffer(subobj, &sub, &n))
        return NULL;

    return stringlib_split((PyObject*) self, s, len, sub, n, maxsplit);
}

PyObject* string_rsplit(PyStringObject* self, PyObject* args) {
    Py_ssize_t len = PyString_GET_SIZE(self), n;
    Py_ssize_t maxsplit = -1;
    const char* s = PyString_AS_STRING(self), *sub;
    PyObject* subobj = Py_None;

    if (!PyArg_ParseTuple(args, "|On:rsplit", &subobj, &maxsplit))
        return NULL;
    if (maxsplit < 0)
        maxsplit = PY_SSIZE_T_MAX;
    if (subobj == Py_None)
        return stringlib_rsplit_whitespace((PyObject*)self, s, len, maxsplit);
    if (PyString_Check(subobj)) {
        sub = PyString_AS_STRING(subobj);
        n = PyString_GET_SIZE(subobj);
    }
#ifdef Py_USING_UNICODE
    else if (PyUnicode_Check(subobj))
        return PyUnicode_RSplit((PyObject*)self, subobj, maxsplit);
#endif
    else if (PyObject_AsCharBuffer(subobj, &sub, &n))
        return NULL;

    return stringlib_rsplit((PyObject*)self, s, len, sub, n, maxsplit);
}

Py_LOCAL_INLINE(Py_ssize_t)
string_find_internal(PyStringObject *self, PyObject *args, int dir)
{
    PyObject *subobj;
    const char *sub;
    Py_ssize_t sub_len;
    Py_ssize_t start=0, end=PY_SSIZE_T_MAX;

    if (!stringlib_parse_args_finds("find/rfind/index/rindex",
                                    args, &subobj, &start, &end))
        return -2;

    if (PyString_Check(subobj)) {
        sub = PyString_AS_STRING(subobj);
        sub_len = PyString_GET_SIZE(subobj);
    }
#ifdef Py_USING_UNICODE
    else if (PyUnicode_Check(subobj))
        return PyUnicode_Find(
            (PyObject *)self, subobj, start, end, dir);
#endif
    else if (PyObject_AsCharBuffer(subobj, &sub, &sub_len))
        /* XXX - the "expected a character buffer object" is pretty
           confusing for a non-expert.  remap to something else ? */
        return -2;

    if (dir > 0)
        return stringlib_find_slice(
            PyString_AS_STRING(self), PyString_GET_SIZE(self),
            sub, sub_len, start, end);
    else
        return stringlib_rfind_slice(
            PyString_AS_STRING(self), PyString_GET_SIZE(self),
            sub, sub_len, start, end);
}

PyObject *
string_rfind(PyStringObject *self, PyObject *args)
{
    Py_ssize_t result = string_find_internal(self, args, -1);
    if (result == -2)
        return NULL;
    return PyInt_FromSsize_t(result);
}

PyObject *
string_find(PyStringObject *self, PyObject *args)
{
    Py_ssize_t result = string_find_internal(self, args, +1);
    if (result == -2)
        return NULL;
    return PyInt_FromSsize_t(result);
}

PyObject *
string_index(PyStringObject *self, PyObject *args)
{
    Py_ssize_t result = string_find_internal(self, args, +1);
    if (result == -2)
        return NULL;
    if (result == -1) {
        PyErr_SetString(PyExc_ValueError,
                        "substring not found");
        return NULL;
    }
    return PyInt_FromSsize_t(result);
}

PyObject *
string_rindex(PyStringObject *self, PyObject *args)
{
    Py_ssize_t result = string_find_internal(self, args, -1);
    if (result == -2)
        return NULL;
    if (result == -1) {
        PyErr_SetString(PyExc_ValueError,
                        "substring not found");
        return NULL;
    }
    return PyInt_FromSsize_t(result);
}

PyObject*
string_splitlines(PyStringObject *self, PyObject *args)
{
    int keepends = 0;

    if (!PyArg_ParseTuple(args, "|i:splitlines", &keepends))
        return NULL;

    return stringlib_splitlines(
        (PyObject*) self, PyString_AS_STRING(self), PyString_GET_SIZE(self),
        keepends
    );
}

PyObject *PyString_AsDecodedObject(PyObject *str,
                                   const char *encoding,
                                   const char *errors)
{
    PyObject *v;

    if (!PyString_Check(str)) {
        PyErr_BadArgument();
        goto onError;
    }

    if (encoding == NULL) {
#ifdef Py_USING_UNICODE
        encoding = PyUnicode_GetDefaultEncoding();
#else
        PyErr_SetString(PyExc_ValueError, "no encoding specified");
        goto onError;
#endif
    }

    /* Decode via the codec registry */
    v = PyCodec_Decode(str, encoding, errors);
    if (v == NULL)
        goto onError;

    return v;

 onError:
    return NULL;
}

PyObject *PyString_AsEncodedObject(PyObject *str,
                                   const char *encoding,
                                   const char *errors)
{
    PyObject *v;

    if (!PyString_Check(str)) {
        PyErr_BadArgument();
        goto onError;
    }

    if (encoding == NULL) {
#ifdef Py_USING_UNICODE
        encoding = PyUnicode_GetDefaultEncoding();
#else
        PyErr_SetString(PyExc_ValueError, "no encoding specified");
        goto onError;
#endif
    }

    /* Encode via the codec registry */
    v = PyCodec_Encode(str, encoding, errors);
    if (v == NULL)
        goto onError;

    return v;

 onError:
    return NULL;
}

PyObject *string_join(PyStringObject *self, PyObject *orig)
{
    char *sep = PyString_AS_STRING(self);
    const Py_ssize_t seplen = PyString_GET_SIZE(self);
    PyObject *res = NULL;
    char *p;
    Py_ssize_t seqlen = 0;
    size_t sz = 0;
    Py_ssize_t i;
    PyObject *seq, *item;

    seq = PySequence_Fast(orig, "");
    if (seq == NULL) {
        return NULL;
    }

    seqlen = PySequence_Size(seq);
    if (seqlen == 0) {
        Py_DECREF(seq);
        return PyString_FromString("");
    }
    if (seqlen == 1) {
        item = PySequence_Fast_GET_ITEM(seq, 0);
        if (PyString_CheckExact(item) || PyUnicode_CheckExact(item)) {
            Py_INCREF(item);
            Py_DECREF(seq);
            return item;
        }
    }

    /* There are at least two things to join, or else we have a subclass
     * of the builtin types in the sequence.
     * Do a pre-pass to figure out the total amount of space we'll
     * need (sz), see whether any argument is absurd, and defer to
     * the Unicode join if appropriate.
     */
    for (i = 0; i < seqlen; i++) {
        const size_t old_sz = sz;
        item = PySequence_Fast_GET_ITEM(seq, i);
        if (!PyString_Check(item)){
#ifdef Py_USING_UNICODE
            if (PyUnicode_Check(item)) {
                /* Defer to Unicode join.
                 * CAUTION:  There's no gurantee that the
                 * original sequence can be iterated over
                 * again, so we must pass seq here.
                 */
                PyObject *result;
                result = PyUnicode_Join((PyObject *)self, seq);
                Py_DECREF(seq);
                return result;
            }
#endif
            PyErr_Format(PyExc_TypeError,
                         "sequence item %zd: expected string,"
                         " %.80s found",
                         i, Py_TYPE(item)->tp_name);
            Py_DECREF(seq);
            return NULL;
        }
        sz += PyString_GET_SIZE(item);
        if (i != 0)
            sz += seplen;
        if (sz < old_sz || sz > PY_SSIZE_T_MAX) {
            PyErr_SetString(PyExc_OverflowError,
                "join() result is too long for a Python string");
            Py_DECREF(seq);
            return NULL;
        }
    }

    /* Allocate result space. */
    res = PyString_FromStringAndSize((char*)NULL, sz);
    if (res == NULL) {
        Py_DECREF(seq);
        return NULL;
    }

    /* Catenate everything. */
    p = PyString_AS_STRING(res);
    for (i = 0; i < seqlen; ++i) {
        size_t n;
        item = PySequence_Fast_GET_ITEM(seq, i);
        n = PyString_GET_SIZE(item);
        Py_MEMCPY(p, PyString_AS_STRING(item), n);
        p += n;
        if (i < seqlen - 1) {
            Py_MEMCPY(p, sep, seplen);
            p += seplen;
        }
    }

    Py_DECREF(seq);
    return res;
}

PyObject* string__format__(PyObject* self, PyObject* args)
{
    PyObject *format_spec;
    PyObject *result = NULL;
    PyObject *tmp = NULL;

    /* If 2.x, convert format_spec to the same type as value */
    /* This is to allow things like u''.format('') */
    if (!PyArg_ParseTuple(args, "O:__format__", &format_spec))
        goto done;
    if (!(PyString_Check(format_spec) || PyUnicode_Check(format_spec))) {
        PyErr_Format(PyExc_TypeError, "__format__ arg must be str "
                     "or unicode, not %s", Py_TYPE(format_spec)->tp_name);
        goto done;
    }
    tmp = PyObject_Str(format_spec);
    if (tmp == NULL)
        goto done;
    format_spec = tmp;

    result = _PyBytes_FormatAdvanced(self,
                                     PyString_AS_STRING(format_spec),
                                     PyString_GET_SIZE(format_spec));
done:
    Py_XDECREF(tmp);
    return result;
}

/* find and count characters and substrings */

#define findchar(target, target_len, c)                         \
  ((char *)memchr((const void *)(target), c, target_len))

/* String ops must return a string.  */
/* If the object is subclass of string, create a copy */
Py_LOCAL(PyStringObject *)
return_self(PyStringObject *self)
{
    if (PyString_CheckExact(self)) {
        Py_INCREF(self);
        return self;
    }
    return (PyStringObject *)PyString_FromStringAndSize(
        PyString_AS_STRING(self),
        PyString_GET_SIZE(self));
}

Py_LOCAL_INLINE(Py_ssize_t)
countchar(const char *target, Py_ssize_t target_len, char c, Py_ssize_t maxcount)
{
    Py_ssize_t count=0;
    const char *start=target;
    const char *end=target+target_len;

    while ( (start=findchar(start, end-start, c)) != NULL ) {
        count++;
        if (count >= maxcount)
            break;
        start += 1;
    }
    return count;
}

/* Algorithms for different cases of string replacement */

/* len(self)>=1, from="", len(to)>=1, maxcount>=1 */
Py_LOCAL(PyStringObject *)
replace_interleave(PyStringObject *self,
                   const char *to_s, Py_ssize_t to_len,
                   Py_ssize_t maxcount)
{
    char *self_s, *result_s;
    Py_ssize_t self_len, result_len;
    Py_ssize_t count, i, product;
    PyStringObject *result;

    self_len = PyString_GET_SIZE(self);

    /* 1 at the end plus 1 after every character */
    count = self_len+1;
    if (maxcount < count)
        count = maxcount;

    /* Check for overflow */
    /*   result_len = count * to_len + self_len; */
    product = count * to_len;
    if (product / to_len != count) {
        PyErr_SetString(PyExc_OverflowError,
                        "replace string is too long");
        return NULL;
    }
    result_len = product + self_len;
    if (result_len < 0) {
        PyErr_SetString(PyExc_OverflowError,
                        "replace string is too long");
        return NULL;
    }

    if (! (result = (PyStringObject *)
                     PyString_FromStringAndSize(NULL, result_len)) )
        return NULL;

    self_s = PyString_AS_STRING(self);
    result_s = PyString_AS_STRING(result);

    /* TODO: special case single character, which doesn't need memcpy */

    /* Lay the first one down (guaranteed this will occur) */
    Py_MEMCPY(result_s, to_s, to_len);
    result_s += to_len;
    count -= 1;

    for (i=0; i<count; i++) {
        *result_s++ = *self_s++;
        Py_MEMCPY(result_s, to_s, to_len);
        result_s += to_len;
    }

    /* Copy the rest of the original string */
    Py_MEMCPY(result_s, self_s, self_len-i);

    return result;
}

/* Special case for deleting a single character */
/* len(self)>=1, len(from)==1, to="", maxcount>=1 */
Py_LOCAL(PyStringObject *)
replace_delete_single_character(PyStringObject *self,
                                char from_c, Py_ssize_t maxcount)
{
    char *self_s, *result_s;
    char *start, *next, *end;
    Py_ssize_t self_len, result_len;
    Py_ssize_t count;
    PyStringObject *result;

    self_len = PyString_GET_SIZE(self);
    self_s = PyString_AS_STRING(self);

    count = countchar(self_s, self_len, from_c, maxcount);
    if (count == 0) {
        return return_self(self);
    }

    result_len = self_len - count;  /* from_len == 1 */
    assert(result_len>=0);

    if ( (result = (PyStringObject *)
                    PyString_FromStringAndSize(NULL, result_len)) == NULL)
        return NULL;
    result_s = PyString_AS_STRING(result);

    start = self_s;
    end = self_s + self_len;
    while (count-- > 0) {
        next = findchar(start, end-start, from_c);
        if (next == NULL)
            break;
        Py_MEMCPY(result_s, start, next-start);
        result_s += (next-start);
        start = next+1;
    }
    Py_MEMCPY(result_s, start, end-start);

    return result;
}

/* len(self)>=1, len(from)>=2, to="", maxcount>=1 */

Py_LOCAL(PyStringObject *)
replace_delete_substring(PyStringObject *self,
                         const char *from_s, Py_ssize_t from_len,
                         Py_ssize_t maxcount) {
    char *self_s, *result_s;
    char *start, *next, *end;
    Py_ssize_t self_len, result_len;
    Py_ssize_t count, offset;
    PyStringObject *result;

    self_len = PyString_GET_SIZE(self);
    self_s = PyString_AS_STRING(self);

    count = stringlib_count(self_s, self_len,
                            from_s, from_len,
                            maxcount);

    if (count == 0) {
        /* no matches */
        return return_self(self);
    }

    result_len = self_len - (count * from_len);
    assert (result_len>=0);

    if ( (result = (PyStringObject *)
          PyString_FromStringAndSize(NULL, result_len)) == NULL )
        return NULL;

    result_s = PyString_AS_STRING(result);

    start = self_s;
    end = self_s + self_len;
    while (count-- > 0) {
        offset = stringlib_find(start, end-start,
                                from_s, from_len,
                                0);
        if (offset == -1)
            break;
        next = start + offset;

        Py_MEMCPY(result_s, start, next-start);

        result_s += (next-start);
        start = next+from_len;
    }
    Py_MEMCPY(result_s, start, end-start);
    return result;
}

/* len(self)>=1, len(from)==len(to)==1, maxcount>=1 */
Py_LOCAL(PyStringObject *)
replace_single_character_in_place(PyStringObject *self,
                                  char from_c, char to_c,
                                  Py_ssize_t maxcount)
{
    char *self_s, *result_s, *start, *end, *next;
    Py_ssize_t self_len;
    PyStringObject *result;

    /* The result string will be the same size */
    self_s = PyString_AS_STRING(self);
    self_len = PyString_GET_SIZE(self);

    next = findchar(self_s, self_len, from_c);

    if (next == NULL) {
        /* No matches; return the original string */
        return return_self(self);
    }

    /* Need to make a new string */
    result = (PyStringObject *) PyString_FromStringAndSize(NULL, self_len);
    if (result == NULL)
        return NULL;
    result_s = PyString_AS_STRING(result);
    Py_MEMCPY(result_s, self_s, self_len);

    /* change everything in-place, starting with this one */
    start =  result_s + (next-self_s);
    *start = to_c;
    start++;
    end = result_s + self_len;

    while (--maxcount > 0) {
        next = findchar(start, end-start, from_c);
        if (next == NULL)
            break;
        *next = to_c;
        start = next+1;
    }

    return result;
}

/* len(self)>=1, len(from)==len(to)>=2, maxcount>=1 */
Py_LOCAL(PyStringObject *)
replace_substring_in_place(PyStringObject *self,
                           const char *from_s, Py_ssize_t from_len,
                           const char *to_s, Py_ssize_t to_len,
                           Py_ssize_t maxcount)
{
    char *result_s, *start, *end;
    char *self_s;
    Py_ssize_t self_len, offset;
    PyStringObject *result;

    /* The result string will be the same size */

    self_s = PyString_AS_STRING(self);
    self_len = PyString_GET_SIZE(self);

    offset = stringlib_find(self_s, self_len,
                            from_s, from_len,
                            0);
    if (offset == -1) {
        /* No matches; return the original string */
        return return_self(self);
    }

    /* Need to make a new string */
    result = (PyStringObject *) PyString_FromStringAndSize(NULL, self_len);
    if (result == NULL)
        return NULL;
    result_s = PyString_AS_STRING(result);
    Py_MEMCPY(result_s, self_s, self_len);

    /* change everything in-place, starting with this one */
    start =  result_s + offset;
    Py_MEMCPY(start, to_s, from_len);
    start += from_len;
    end = result_s + self_len;

    while ( --maxcount > 0) {
        offset = stringlib_find(start, end-start,
                                from_s, from_len,
                                0);
        if (offset==-1)
            break;
        Py_MEMCPY(start+offset, to_s, from_len);
        start += offset+from_len;
    }

    return result;
}

/* len(self)>=1, len(from)==1, len(to)>=2, maxcount>=1 */
Py_LOCAL(PyStringObject *)
replace_single_character(PyStringObject *self,
                         char from_c,
                         const char *to_s, Py_ssize_t to_len,
                         Py_ssize_t maxcount)
{
    char *self_s, *result_s;
    char *start, *next, *end;
    Py_ssize_t self_len, result_len;
    Py_ssize_t count, product;
    PyStringObject *result;

    self_s = PyString_AS_STRING(self);
    self_len = PyString_GET_SIZE(self);

    count = countchar(self_s, self_len, from_c, maxcount);
    if (count == 0) {
        /* no matches, return unchanged */
        return return_self(self);
    }

    /* use the difference between current and new, hence the "-1" */
    /*   result_len = self_len + count * (to_len-1)  */
    product = count * (to_len-1);
    if (product / (to_len-1) != count) {
        PyErr_SetString(PyExc_OverflowError, "replace string is too long");
        return NULL;
    }
    result_len = self_len + product;
    if (result_len < 0) {
        PyErr_SetString(PyExc_OverflowError, "replace string is too long");
        return NULL;
    }

    if ( (result = (PyStringObject *)
          PyString_FromStringAndSize(NULL, result_len)) == NULL)
        return NULL;
    result_s = PyString_AS_STRING(result);

    start = self_s;
    end = self_s + self_len;
    while (count-- > 0) {
        next = findchar(start, end-start, from_c);
        if (next == NULL)
            break;

        if (next == start) {
            /* replace with the 'to' */
            Py_MEMCPY(result_s, to_s, to_len);
            result_s += to_len;
            start += 1;
        } else {
            /* copy the unchanged old then the 'to' */
            Py_MEMCPY(result_s, start, next-start);
            result_s += (next-start);
            Py_MEMCPY(result_s, to_s, to_len);
            result_s += to_len;
            start = next+1;
        }
    }
    /* Copy the remainder of the remaining string */
    Py_MEMCPY(result_s, start, end-start);

    return result;
}

/* len(self)>=1, len(from)>=2, len(to)>=2, maxcount>=1 */
Py_LOCAL(PyStringObject *)
replace_substring(PyStringObject *self,
                  const char *from_s, Py_ssize_t from_len,
                  const char *to_s, Py_ssize_t to_len,
                  Py_ssize_t maxcount) {
    char *self_s, *result_s;
    char *start, *next, *end;
    Py_ssize_t self_len, result_len;
    Py_ssize_t count, offset, product;
    PyStringObject *result;

    self_s = PyString_AS_STRING(self);
    self_len = PyString_GET_SIZE(self);

    count = stringlib_count(self_s, self_len,
                            from_s, from_len,
                            maxcount);

    if (count == 0) {
        /* no matches, return unchanged */
        return return_self(self);
    }

    /* Check for overflow */
    /*    result_len = self_len + count * (to_len-from_len) */
    product = count * (to_len-from_len);
    if (product / (to_len-from_len) != count) {
        PyErr_SetString(PyExc_OverflowError, "replace string is too long");
        return NULL;
    }
    result_len = self_len + product;
    if (result_len < 0) {
        PyErr_SetString(PyExc_OverflowError, "replace string is too long");
        return NULL;
    }

    if ( (result = (PyStringObject *)
          PyString_FromStringAndSize(NULL, result_len)) == NULL)
        return NULL;
    result_s = PyString_AS_STRING(result);

    start = self_s;
    end = self_s + self_len;
    while (count-- > 0) {
        offset = stringlib_find(start, end-start,
                                from_s, from_len,
                                0);
        if (offset == -1)
            break;
        next = start+offset;
        if (next == start) {
            /* replace with the 'to' */
            Py_MEMCPY(result_s, to_s, to_len);
            result_s += to_len;
            start += from_len;
        } else {
            /* copy the unchanged old then the 'to' */
            Py_MEMCPY(result_s, start, next-start);
            result_s += (next-start);
            Py_MEMCPY(result_s, to_s, to_len);
            result_s += to_len;
            start = next+from_len;
        }
    }
    /* Copy the remainder of the remaining string */
    Py_MEMCPY(result_s, start, end-start);

    return result;
}


Py_LOCAL(PyStringObject *)
replace(PyStringObject *self,
    const char *from_s, Py_ssize_t from_len,
    const char *to_s, Py_ssize_t to_len,
    Py_ssize_t maxcount)
{
    if (maxcount < 0) {
        maxcount = PY_SSIZE_T_MAX;
    } else if (maxcount == 0 || PyString_GET_SIZE(self) == 0) {
        /* nothing to do; return the original string */
        return return_self(self);
    }

    if (maxcount == 0 ||
        (from_len == 0 && to_len == 0)) {
        /* nothing to do; return the original string */
        return return_self(self);
    }

    /* Handle zero-length special cases */

    if (from_len == 0) {
        /* insert the 'to' string everywhere.   */
        /*    >>> "Python".replace("", ".")     */
        /*    '.P.y.t.h.o.n.'                   */
        return replace_interleave(self, to_s, to_len, maxcount);
    }

    /* Except for "".replace("", "A") == "A" there is no way beyond this */
    /* point for an empty self string to generate a non-empty string */
    /* Special case so the remaining code always gets a non-empty string */
    if (PyString_GET_SIZE(self) == 0) {
        return return_self(self);
    }

    if (to_len == 0) {
        /* delete all occurances of 'from' string */
        if (from_len == 1) {
            return replace_delete_single_character(
                self, from_s[0], maxcount);
        } else {
            return replace_delete_substring(self, from_s, from_len, maxcount);
        }
    }

    /* Handle special case where both strings have the same length */

    if (from_len == to_len) {
        if (from_len == 1) {
            return replace_single_character_in_place(
                self,
                from_s[0],
                to_s[0],
                maxcount);
        } else {
            return replace_substring_in_place(
                self, from_s, from_len, to_s, to_len, maxcount);
        }
    }

    /* Otherwise use the more generic algorithms */
    if (from_len == 1) {
        return replace_single_character(self, from_s[0],
                                        to_s, to_len, maxcount);
    } else {
        /* len('from')>=2, len('to')>=1 */
        return replace_substring(self, from_s, from_len, to_s, to_len, maxcount);
    }
}

// Pyston change: don't use varags calling convention
// PyObject* string_replace(PyStringObject *self, PyObject *args)
PyObject* string_replace(PyStringObject *self, PyObject *from, PyObject* to, PyObject** args)
{
    PyObject* _count = args[0];
    Py_ssize_t count = -1;
    const char *from_s, *to_s;
    Py_ssize_t from_len, to_len;

    // Pyston change: don't use varags calling convention
    // if (!PyArg_ParseTuple(args, "OO|n:replace", &from, &to, &count))
    //    return NULL;
    if (_count && !PyArg_ParseSingle(_count, 3, "replace", "n", &count))
        return NULL;

    if (PyString_Check(from)) {
        from_s = PyString_AS_STRING(from);
        from_len = PyString_GET_SIZE(from);
    }
#ifdef Py_USING_UNICODE
    if (PyUnicode_Check(from))
        return PyUnicode_Replace((PyObject *)self,
                                 from, to, count);
#endif
    else if (PyObject_AsCharBuffer(from, &from_s, &from_len))
        return NULL;

    if (PyString_Check(to)) {
        to_s = PyString_AS_STRING(to);
        to_len = PyString_GET_SIZE(to);
    }
#ifdef Py_USING_UNICODE
    else if (PyUnicode_Check(to))
        return PyUnicode_Replace((PyObject *)self,
                                 from, to, count);
#endif
    else if (PyObject_AsCharBuffer(to, &to_s, &to_len))
        return NULL;

    return (PyObject *)replace((PyStringObject *) self,
                               from_s, from_len,
                               to_s, to_len, count);
}
