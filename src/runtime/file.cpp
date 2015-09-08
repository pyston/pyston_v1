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

#include "runtime/file.h"

#include <cstdio>
#include <cstring>
#include <sstream>

#include "capi/types.h"
#include "core/common.h"
#include "core/stats.h"
#include "core/types.h"
#include "runtime/objmodel.h"
#include "runtime/types.h"
#include "runtime/util.h"

namespace pyston {

#define BUF(v) PyString_AS_STRING((PyStringObject*)v)

#ifdef HAVE_GETC_UNLOCKED
#define GETC(f) getc_unlocked(f)
#define FLOCKFILE(f) flockfile(f)
#define FUNLOCKFILE(f) funlockfile(f)
#else
#define GETC(f) getc(f)
#define FLOCKFILE(f)
#define FUNLOCKFILE(f)
#endif

/* Bits in f_newlinetypes */
#define NEWLINE_UNKNOWN 0 /* No newline seen, yet */
#define NEWLINE_CR 1      /* \r newline seen */
#define NEWLINE_LF 2      /* \n newline seen */
#define NEWLINE_CRLF 4    /* \r\n newline seen */

#define FILE_BEGIN_ALLOW_THREADS(fobj)                                                                                 \
    {                                                                                                                  \
        fobj->unlocked_count++;                                                                                        \
    Py_BEGIN_ALLOW_THREADS

#define FILE_END_ALLOW_THREADS(fobj)                                                                                   \
    Py_END_ALLOW_THREADS fobj->unlocked_count--;                                                                       \
    assert(fobj->unlocked_count >= 0);                                                                                 \
    }

#define FILE_ABORT_ALLOW_THREADS(fobj)                                                                                 \
    Py_BLOCK_THREADS fobj->unlocked_count--;                                                                           \
    assert(fobj->unlocked_count >= 0);

#if BUFSIZ < 8192
#define SMALLCHUNK 8192
#else
#define SMALLCHUNK BUFSIZ
#endif

static size_t new_buffersize(BoxedFile* f, size_t currentsize) {
#ifdef HAVE_FSTAT
    off_t pos, end;
    struct stat st;
    if (fstat(fileno(f->f_fp), &st) == 0) {
        end = st.st_size;
        /* The following is not a bug: we really need to call lseek()
           *and* ftell().  The reason is that some stdio libraries
           mistakenly flush their buffer when ftell() is called and
           the lseek() call it makes fails, thereby throwing away
           data that cannot be recovered in any way.  To avoid this,
           we first test lseek(), and only call ftell() if lseek()
           works.  We can't use the lseek() value either, because we
           need to take the amount of buffered data into account.
           (Yet another reason why stdio stinks. :-) */
        pos = lseek(fileno(f->f_fp), 0L, SEEK_CUR);
        if (pos >= 0) {
            pos = ftell(f->f_fp);
        }
        if (pos < 0)
            clearerr(f->f_fp);
        if (end > pos && pos >= 0)
            return currentsize + end - pos + 1;
        /* Add 1 so if the file were to grow we'd notice. */
    }
#endif
    /* Expand the buffer by an amount proportional to the current size,
       giving us amortized linear-time behavior. Use a less-than-double
       growth factor to avoid excessive allocation. */
    return currentsize + (currentsize >> 3) + 6;
}

#if defined(EWOULDBLOCK) && defined(EAGAIN) && EWOULDBLOCK != EAGAIN
#define BLOCKED_ERRNO(x) ((x) == EWOULDBLOCK || (x) == EAGAIN)
#else
#ifdef EWOULDBLOCK
#define BLOCKED_ERRNO(x) ((x) == EWOULDBLOCK)
#else
#ifdef EAGAIN
#define BLOCKED_ERRNO(x) ((x) == EAGAIN)
#else
#define BLOCKED_ERRNO(x) 0
#endif
#endif
#endif

static PyObject* err_closed(void) noexcept {
    PyErr_SetString(PyExc_ValueError, "I/O operation on closed file");
    return NULL;
}

static PyObject* err_mode(const char* action) noexcept {
    PyErr_Format(PyExc_IOError, "File not open for %s", action);
    return NULL;
}

/* Refuse regular file I/O if there's data in the iteration-buffer.
 * Mixing them would cause data to arrive out of order, as the read*
 * methods don't use the iteration buffer. */
static PyObject* err_iterbuffered(void) noexcept {
    PyErr_SetString(PyExc_ValueError, "Mixing iteration and read methods would lose data");
    return NULL;
}

static BoxedFile* dircheck(BoxedFile* f) {
#if defined(HAVE_FSTAT) && defined(S_IFDIR) && defined(EISDIR)
    struct stat buf;
    if (f->f_fp == NULL)
        return f;
    if (fstat(fileno(f->f_fp), &buf) == 0 && S_ISDIR(buf.st_mode)) {
        char* msg = strerror(EISDIR);
        PyObject* exc = PyObject_CallFunction(PyExc_IOError, "(isO)", EISDIR, msg, f->f_name);
        PyErr_SetObject(PyExc_IOError, exc);
        Py_XDECREF(exc);
        return NULL;
    }
#endif
    return f;
}

static PyObject* fill_file_fields(BoxedFile* f, FILE* fp, PyObject* name, const char* mode, int (*close)(FILE*)) {
    assert(name != NULL);
    assert(f != NULL);
    assert(PyFile_Check(f));
    assert(f->f_fp == NULL);

    Py_DECREF(f->f_name);
    Py_DECREF(f->f_mode);
    Py_DECREF(f->f_encoding);
    Py_DECREF(f->f_errors);

    Py_INCREF(name);
    f->f_name = name;

    f->f_mode = PyString_FromString(mode);

    f->f_close = close;
    f->f_softspace = 0;
    f->f_binary = strchr(mode, 'b') != NULL;
    f->f_buf = NULL;
    f->f_univ_newline = (strchr(mode, 'U') != NULL);
    f->f_newlinetypes = NEWLINE_UNKNOWN;
    f->f_skipnextlf = 0;
    Py_INCREF(Py_None);
    f->f_encoding = Py_None;
    Py_INCREF(Py_None);
    f->f_errors = Py_None;
    f->readable = f->writable = 0;
    if (strchr(mode, 'r') != NULL || f->f_univ_newline)
        f->readable = 1;
    if (strchr(mode, 'w') != NULL || strchr(mode, 'a') != NULL)
        f->writable = 1;
    if (strchr(mode, '+') != NULL)
        f->readable = f->writable = 1;

    if (f->f_mode == NULL)
        return NULL;
    f->f_fp = fp;
    f = dircheck(f);
    return (PyObject*)f;
}

BoxedFile::BoxedFile(FILE* f, std::string fname, const char* fmode, int (*close)(FILE*))
    // Zero out fields not set by fill_file_fields:
    : f_fp(NULL),
      f_bufend(NULL),
      f_bufptr(0),
      f_setbuf(0),
      unlocked_count(0) {
    Box* r = fill_file_fields(this, f, boxString(fname), fmode, close);
    checkAndThrowCAPIException();
    assert(r == this);
}

Box* fileRepr(BoxedFile* self) {
    assert(self->cls == file_cls);

    void* addr = static_cast<void*>(self->f_fp);
    std::ostringstream repr;

    repr << "<" << (self->f_fp ? "open" : "closed") << " file '" << PyString_AsString(self->f_name) << "', ";
    repr << "mode '" << PyString_AsString(self->f_mode) << "' at " << addr << ">";

    return boxString(repr.str());
}

static void checkOpen(BoxedFile* self) {
    if (!self->f_fp)
        raiseExcHelper(IOError, "I/O operation on closed file");
}

static void checkReadable(BoxedFile* self) {
    checkOpen(self);
    if (!self->readable)
        raiseExcHelper(IOError, "File not open for reading");
}

static void checkWritable(BoxedFile* self) {
    checkOpen(self);
    if (!self->writable)
        raiseExcHelper(IOError, "File not open for writing");
}

static PyObject* file_read(BoxedFile* f, long bytesrequested) noexcept {
    size_t bytesread, buffersize, chunksize;
    PyObject* v;

    if (f->f_fp == NULL)
        return err_closed();
    if (!f->readable)
        return err_mode("reading");
    /* refuse to mix with f.next() */
    if (f->f_buf != NULL && (f->f_bufend - f->f_bufptr) > 0 && f->f_buf[0] != '\0')
        return err_iterbuffered();

    if (bytesrequested < 0)
        buffersize = new_buffersize(f, (size_t)0);
    else
        buffersize = bytesrequested;
    if (buffersize > PY_SSIZE_T_MAX) {
        PyErr_SetString(PyExc_OverflowError, "requested number of bytes is more than a Python string can hold");
        return NULL;
    }
    v = PyString_FromStringAndSize((char*)NULL, buffersize);
    if (v == NULL)
        return NULL;
    bytesread = 0;
    for (;;) {
        int interrupted;
        FILE_BEGIN_ALLOW_THREADS(f)
        errno = 0;
        chunksize = Py_UniversalNewlineFread(BUF(v) + bytesread, buffersize - bytesread, f->f_fp, (PyObject*)f);
        interrupted = ferror(f->f_fp) && errno == EINTR;
        FILE_END_ALLOW_THREADS(f)
        if (interrupted) {
            clearerr(f->f_fp);
            if (PyErr_CheckSignals()) {
                Py_DECREF(v);
                return NULL;
            }
        }
        if (chunksize == 0) {
            if (interrupted)
                continue;
            if (!ferror(f->f_fp))
                break;
            clearerr(f->f_fp);
            /* When in non-blocking mode, data shouldn't
             * be discarded if a blocking signal was
             * received. That will also happen if
             * chunksize != 0, but bytesread < buffersize. */
            if (bytesread > 0 && BLOCKED_ERRNO(errno))
                break;
            PyErr_SetFromErrno(PyExc_IOError);
            Py_DECREF(v);
            return NULL;
        }
        bytesread += chunksize;
        if (bytesread < buffersize && !interrupted) {
            clearerr(f->f_fp);
            break;
        }
        if (bytesrequested < 0) {
            buffersize = new_buffersize(f, buffersize);
            if (_PyString_Resize(&v, buffersize) < 0)
                return NULL;
        } else {
            /* Got what was requested. */
            break;
        }
    }
    if (bytesread != buffersize && _PyString_Resize(&v, bytesread))
        return NULL;
    return v;
}

static PyObject* get_line(BoxedFile* f, int n) noexcept {
    FILE* fp = f->f_fp;
    int c;
    char* buf, *end;
    size_t total_v_size; /* total # of slots in buffer */
    size_t used_v_size;  /* # used slots in buffer */
    size_t increment;    /* amount to increment the buffer */
    PyObject* v;
    int newlinetypes = f->f_newlinetypes;
    int skipnextlf = f->f_skipnextlf;
    int univ_newline = f->f_univ_newline;

#if defined(USE_FGETS_IN_GETLINE)
    if (n <= 0 && !univ_newline)
        return getline_via_fgets(f, fp);
#endif
    total_v_size = n > 0 ? n : 100;
    v = PyString_FromStringAndSize((char*)NULL, total_v_size);
    if (v == NULL)
        return NULL;
    buf = BUF(v);
    end = buf + total_v_size;

    for (;;) {
        FILE_BEGIN_ALLOW_THREADS(f)
        FLOCKFILE(fp);
        if (univ_newline) {
            c = 'x'; /* Shut up gcc warning */
            while (buf != end && (c = GETC(fp)) != EOF) {
                if (skipnextlf) {
                    skipnextlf = 0;
                    if (c == '\n') {
                        /* Seeing a \n here with
                         * skipnextlf true means we
                         * saw a \r before.
                         */
                        newlinetypes |= NEWLINE_CRLF;
                        c = GETC(fp);
                        if (c == EOF)
                            break;
                    } else {
                        newlinetypes |= NEWLINE_CR;
                    }
                }
                if (c == '\r') {
                    skipnextlf = 1;
                    c = '\n';
                } else if (c == '\n')
                    newlinetypes |= NEWLINE_LF;
                *buf++ = c;
                if (c == '\n')
                    break;
            }
            if (c == EOF) {
                if (ferror(fp) && errno == EINTR) {
                    FUNLOCKFILE(fp);
                    FILE_ABORT_ALLOW_THREADS(f)
                    f->f_newlinetypes = newlinetypes;
                    f->f_skipnextlf = skipnextlf;

                    if (PyErr_CheckSignals()) {
                        Py_DECREF(v);
                        return NULL;
                    }
                    /* We executed Python signal handlers and got no exception.
                     * Now back to reading the line where we left off. */
                    clearerr(fp);
                    continue;
                }
                if (skipnextlf)
                    newlinetypes |= NEWLINE_CR;
            }
        } else /* If not universal newlines use the normal loop */
            while ((c = GETC(fp)) != EOF && (*buf++ = c) != '\n' && buf != end)
                ;
        FUNLOCKFILE(fp);
        FILE_END_ALLOW_THREADS(f)
        f->f_newlinetypes = newlinetypes;
        f->f_skipnextlf = skipnextlf;
        if (c == '\n')
            break;
        if (c == EOF) {
            if (ferror(fp)) {
                if (errno == EINTR) {
                    if (PyErr_CheckSignals()) {
                        Py_DECREF(v);
                        return NULL;
                    }
                    /* We executed Python signal handlers and got no exception.
                     * Now back to reading the line where we left off. */
                    clearerr(fp);
                    continue;
                }
                PyErr_SetFromErrno(PyExc_IOError);
                clearerr(fp);
                Py_DECREF(v);
                return NULL;
            }
            clearerr(fp);
            if (PyErr_CheckSignals()) {
                Py_DECREF(v);
                return NULL;
            }
            break;
        }
        /* Must be because buf == end */
        if (n > 0)
            break;
        used_v_size = total_v_size;
        increment = total_v_size >> 2; /* mild exponential growth */
        total_v_size += increment;
        if (total_v_size > PY_SSIZE_T_MAX) {
            PyErr_SetString(PyExc_OverflowError, "line is longer than a Python string can hold");
            Py_DECREF(v);
            return NULL;
        }
        if (_PyString_Resize(&v, total_v_size) < 0)
            return NULL;
        buf = BUF(v) + used_v_size;
        end = BUF(v) + total_v_size;
    }

    used_v_size = buf - BUF(v);
    if (used_v_size != total_v_size && _PyString_Resize(&v, used_v_size))
        return NULL;
    return v;
}

Box* fileRead(BoxedFile* self, Box* _size) {
    assert(self->cls == file_cls);
    if (_size->cls != int_cls) {
        raiseExcHelper(TypeError, "an integer is required");
    }
    int64_t size = static_cast<BoxedInt*>(_size)->n;

    Box* r = file_read(self, size);
    if (!r)
        throwCAPIException();
    return r;
}

static PyObject* file_readline(BoxedFile* f, int n = -1) noexcept {
    if (f->f_fp == NULL)
        return err_closed();
    if (!f->readable)
        return err_mode("reading");
    /* refuse to mix with f.next() */
    if (f->f_buf != NULL && (f->f_bufend - f->f_bufptr) > 0 && f->f_buf[0] != '\0')
        return err_iterbuffered();
    if (n == 0)
        return PyString_FromString("");
    if (n < 0)
        n = 0;
    return get_line(f, n);
}

Box* fileReadline1(BoxedFile* self) {
    assert(self->cls == file_cls);

    Box* r = file_readline(self);
    if (!r)
        throwCAPIException();
    return r;
}

static PyObject* file_write(BoxedFile* f, Box* arg) noexcept {
    Py_buffer pbuf;
    const char* s;
    Py_ssize_t n, n2;
    PyObject* encoded = NULL;
    int err_flag = 0, err;

    if (f->f_fp == NULL)
        return err_closed();
    if (!f->writable)
        return err_mode("writing");
    if (f->f_binary) {
        // NOTE: this call will create a new tuple every time we write to a binary file. if/when this becomes hot or
        // creates too much GC pressure, we can fix it by adding a Pyston specific versino of PyArg_ParseTuple that
        // (instead of taking a tuple) takes length + Box**.  Then we'd call that directly here (passing "1, &arg").
        if (!PyArg_ParseTuple(BoxedTuple::create({ arg }), "s*", &pbuf))
            return NULL;
        s = (const char*)pbuf.buf;
        n = pbuf.len;
    } else {
        PyObject* text = arg;

        if (PyString_Check(text)) {
            s = PyString_AS_STRING(text);
            n = PyString_GET_SIZE(text);
#ifdef Py_USING_UNICODE
        } else if (PyUnicode_Check(text)) {
            const char* encoding, *errors;
            if (f->f_encoding != Py_None)
                encoding = PyString_AS_STRING(f->f_encoding);
            else
                encoding = PyUnicode_GetDefaultEncoding();
            if (f->f_errors != Py_None)
                errors = PyString_AS_STRING(f->f_errors);
            else
                errors = "strict";
            encoded = PyUnicode_AsEncodedString(text, encoding, errors);
            if (encoded == NULL)
                return NULL;
            s = PyString_AS_STRING(encoded);
            n = PyString_GET_SIZE(encoded);
#endif
        } else {
            if (PyObject_AsCharBuffer(text, &s, &n))
                return NULL;
        }
    }
    // TODO: this doesn't seem like it should be a necessary Pyston change:
    // f->f_softspace = 0;

    FILE_BEGIN_ALLOW_THREADS(f)
    errno = 0;
    n2 = fwrite(s, 1, n, f->f_fp);
    if (n2 != n || ferror(f->f_fp)) {
        err_flag = 1;
        err = errno;
    }
    FILE_END_ALLOW_THREADS(f)
    Py_XDECREF(encoded);
    if (f->f_binary)
        PyBuffer_Release(&pbuf);
    if (err_flag) {
        errno = err;
        PyErr_SetFromErrno(PyExc_IOError);
        clearerr(f->f_fp);
        return NULL;
    }
    Py_INCREF(Py_None);
    return Py_None;
}

static PyObject* file_writelines(BoxedFile* f, PyObject* seq) noexcept {
#define CHUNKSIZE 1000
    PyObject* list, *line;
    PyObject* it; /* iter(seq) */
    PyObject* result;
    int index, islist;
    Py_ssize_t i, j, nwritten, len;

    assert(seq != NULL);
    if (f->f_fp == NULL)
        return err_closed();
    if (!f->writable)
        return err_mode("writing");

    result = NULL;
    list = NULL;
    islist = PyList_Check(seq);
    if (islist)
        it = NULL;
    else {
        it = PyObject_GetIter(seq);
        if (it == NULL) {
            PyErr_SetString(PyExc_TypeError, "writelines() requires an iterable argument");
            return NULL;
        }
        /* From here on, fail by going to error, to reclaim "it". */
        list = PyList_New(CHUNKSIZE);
        if (list == NULL)
            goto error;
    }

    /* Strategy: slurp CHUNKSIZE lines into a private list,
       checking that they are all strings, then write that list
       without holding the interpreter lock, then come back for more. */
    for (index = 0;; index += CHUNKSIZE) {
        if (islist) {
            Py_XDECREF(list);
            list = PyList_GetSlice(seq, index, index + CHUNKSIZE);
            if (list == NULL)
                goto error;
            j = PyList_GET_SIZE(list);
        } else {
            for (j = 0; j < CHUNKSIZE; j++) {
                line = PyIter_Next(it);
                if (line == NULL) {
                    if (PyErr_Occurred())
                        goto error;
                    break;
                }
                PyList_SetItem(list, j, line);
            }
            /* The iterator might have closed the file on us. */
            if (f->f_fp == NULL) {
                err_closed();
                goto error;
            }
        }
        if (j == 0)
            break;

        /* Check that all entries are indeed strings. If not,
           apply the same rules as for file.write() and
           convert the results to strings. This is slow, but
           seems to be the only way since all conversion APIs
           could potentially execute Python code. */
        for (i = 0; i < j; i++) {
            PyObject* v = PyList_GET_ITEM(list, i);
            if (!PyString_Check(v)) {
                const char* buffer;
                int res;
                if (f->f_binary) {
                    res = PyObject_AsReadBuffer(v, (const void**)&buffer, &len);
                } else {
                    res = PyObject_AsCharBuffer(v, &buffer, &len);
                }
                if (res) {
                    PyErr_SetString(PyExc_TypeError, "writelines() argument must be a sequence of strings");
                    goto error;
                }
                line = PyString_FromStringAndSize(buffer, len);
                if (line == NULL)
                    goto error;
                Py_DECREF(v);
                PyList_SET_ITEM(list, i, line);
            }
        }

        /* Since we are releasing the global lock, the
           following code may *not* execute Python code. */
        f->f_softspace = 0;
        FILE_BEGIN_ALLOW_THREADS(f)
        errno = 0;
        for (i = 0; i < j; i++) {
            line = PyList_GET_ITEM(list, i);
            len = PyString_GET_SIZE(line);
            nwritten = fwrite(PyString_AS_STRING(line), 1, len, f->f_fp);
            if (nwritten != len) {
                FILE_ABORT_ALLOW_THREADS(f)
                PyErr_SetFromErrno(PyExc_IOError);
                clearerr(f->f_fp);
                goto error;
            }
        }
        FILE_END_ALLOW_THREADS(f)

        if (j < CHUNKSIZE)
            break;
    }

    Py_INCREF(Py_None);
    result = Py_None;
error:
    Py_XDECREF(list);
    Py_XDECREF(it);
    return result;
#undef CHUNKSIZE
}

Box* fileWrite(BoxedFile* self, Box* val) {
    assert(self->cls == file_cls);

    Box* r = file_write(self, val);
    if (!r)
        throwCAPIException();
    return r;
}

static PyObject* file_flush(BoxedFile* f) noexcept {
    int res;

    if (f->f_fp == NULL)
        return err_closed();
    FILE_BEGIN_ALLOW_THREADS(f)
    errno = 0;
    res = fflush(f->f_fp);
    FILE_END_ALLOW_THREADS(f)
    if (res != 0) {
        PyErr_SetFromErrno(PyExc_IOError);
        clearerr(f->f_fp);
        return NULL;
    }
    Py_INCREF(Py_None);
    return Py_None;
}

Box* fileFlush(BoxedFile* self) {
    RELEASE_ASSERT(self->cls == file_cls, "");

    Box* r = file_flush(self);
    if (!r)
        throwCAPIException();
    return r;
}

static PyObject* close_the_file(BoxedFile* f) {
    int sts = 0;
    int (*local_close)(FILE*);
    FILE* local_fp = f->f_fp;
    char* local_setbuf = f->f_setbuf;
    if (local_fp != NULL) {
        local_close = f->f_close;
        if (local_close != NULL && f->unlocked_count > 0) {
            PyErr_SetString(PyExc_IOError, "close() called during concurrent "
                                           "operation on the same file object.");
            return NULL;
        }
        /* NULL out the FILE pointer before releasing the GIL, because
         * it will not be valid anymore after the close() function is
         * called. */
        f->f_fp = NULL;
        if (local_close != NULL) {
            /* Issue #9295: must temporarily reset f_setbuf so that another
               thread doesn't free it when running file_close() concurrently.
               Otherwise this close() will crash when flushing the buffer. */
            f->f_setbuf = NULL;
            Py_BEGIN_ALLOW_THREADS errno = 0;
            sts = (*local_close)(local_fp);
            Py_END_ALLOW_THREADS f->f_setbuf = local_setbuf;
            if (sts == EOF)
                return PyErr_SetFromErrno(PyExc_IOError);
            if (sts != 0)
                return PyInt_FromLong((long)sts);
        }
    }
    Py_RETURN_NONE;
}

/* Our very own off_t-like type, 64-bit if possible */
#if !defined(HAVE_LARGEFILE_SUPPORT)
typedef off_t Py_off_t;
#elif SIZEOF_OFF_T >= 8
typedef off_t Py_off_t;
#elif SIZEOF_FPOS_T >= 8
typedef fpos_t Py_off_t;
#else
#error "Large file support, but neither off_t nor fpos_t is large enough."
#endif

/* a portable fseek() function
   return 0 on success, non-zero on failure (with errno set) */
static int _portable_fseek(FILE* fp, Py_off_t offset, int whence) {
#if !defined(HAVE_LARGEFILE_SUPPORT)
    return fseek(fp, offset, whence);
#elif defined(HAVE_FSEEKO) && SIZEOF_OFF_T >= 8
    return fseeko(fp, offset, whence);
#elif defined(HAVE_FSEEK64)
    return fseek64(fp, offset, whence);
#elif defined(__BEOS__)
    return _fseek(fp, offset, whence);
#elif SIZEOF_FPOS_T >= 8
    /* lacking a 64-bit capable fseek(), use a 64-bit capable fsetpos()
       and fgetpos() to implement fseek()*/
    fpos_t pos;
    switch (whence) {
        case SEEK_END:
#ifdef MS_WINDOWS
            fflush(fp);
            if (_lseeki64(fileno(fp), 0, 2) == -1)
                return -1;
#else
            if (fseek(fp, 0, SEEK_END) != 0)
                return -1;
#endif
        /* fall through */
        case SEEK_CUR:
            if (fgetpos(fp, &pos) != 0)
                return -1;
            offset += pos;
            break;
            /* case SEEK_SET: break; */
    }
    return fsetpos(fp, &offset);
#else
#error "Large file support, but no way to fseek."
#endif
}

static void drop_readahead(BoxedFile* f) {
    if (f->f_buf != NULL) {
        PyMem_Free(f->f_buf);
        f->f_buf = NULL;
    }
}

static PyObject* file_seek(BoxedFile* f, PyObject* args) {
    int whence;
    int ret;
    Py_off_t offset;
    PyObject* offobj, *off_index;

    if (f->f_fp == NULL)
        return err_closed();
    drop_readahead(f);
    whence = 0;
    if (!PyArg_ParseTuple(args, "O|i:seek", &offobj, &whence))
        return NULL;
    off_index = PyNumber_Index(offobj);
    if (!off_index) {
        if (!PyFloat_Check(offobj))
            return NULL;
        /* Deprecated in 2.6 */
        PyErr_Clear();
        if (PyErr_WarnEx(PyExc_DeprecationWarning, "integer argument expected, got float", 1) < 0)
            return NULL;
        off_index = offobj;
        Py_INCREF(offobj);
    }
#if !defined(HAVE_LARGEFILE_SUPPORT)
    offset = PyInt_AsLong(off_index);
#else
    offset = PyLong_Check(off_index) ? PyLong_AsLongLong(off_index) : PyInt_AsLong(off_index);
#endif
    Py_DECREF(off_index);
    if (PyErr_Occurred())
        return NULL;

    FILE_BEGIN_ALLOW_THREADS(f)
    errno = 0;
    ret = _portable_fseek(f->f_fp, offset, whence);
    FILE_END_ALLOW_THREADS(f)

    if (ret != 0) {
        PyErr_SetFromErrno(PyExc_IOError);
        clearerr(f->f_fp);
        return NULL;
    }
    f->f_skipnextlf = 0;
    Py_INCREF(Py_None);
    return Py_None;
}

/* a portable ftell() function
   Return -1 on failure with errno set appropriately, current file
   position on success */
static Py_off_t _portable_ftell(FILE* fp) {
#if !defined(HAVE_LARGEFILE_SUPPORT)
    return ftell(fp);
#elif defined(HAVE_FTELLO) && SIZEOF_OFF_T >= 8
    return ftello(fp);
#elif defined(HAVE_FTELL64)
    return ftell64(fp);
#elif SIZEOF_FPOS_T >= 8
    fpos_t pos;
    if (fgetpos(fp, &pos) != 0)
        return -1;
    return pos;
#else
#error "Large file support, but no way to ftell."
#endif
}

static PyObject* file_tell(BoxedFile* f) {
    Py_off_t pos;

    if (f->f_fp == NULL)
        return err_closed();
    FILE_BEGIN_ALLOW_THREADS(f)
    errno = 0;
    pos = _portable_ftell(f->f_fp);
    FILE_END_ALLOW_THREADS(f)

    if (pos == -1) {
        PyErr_SetFromErrno(PyExc_IOError);
        clearerr(f->f_fp);
        return NULL;
    }
    if (f->f_skipnextlf) {
        int c;
        c = GETC(f->f_fp);
        if (c == '\n') {
            f->f_newlinetypes |= NEWLINE_CRLF;
            pos++;
            f->f_skipnextlf = 0;
        } else if (c != EOF)
            ungetc(c, f->f_fp);
    }
#if !defined(HAVE_LARGEFILE_SUPPORT)
    return PyInt_FromLong(pos);
#else
    return PyLong_FromLongLong(pos);
#endif
}

Box* fileTell(BoxedFile* f) {
    if (!isSubclass(f->cls, file_cls))
        raiseExcHelper(TypeError, "descriptor 'tell' requires a 'file' object but received a '%s'", getTypeName(f));

    auto rtn = file_tell(f);
    checkAndThrowCAPIException();
    return rtn;
}

Box* fileClose(BoxedFile* self) {
    assert(self->cls == file_cls);

    PyObject* sts = close_the_file(self);
    if (sts) {
        PyMem_Free(self->f_setbuf);
        self->f_setbuf = NULL;
    } else {
        throwCAPIException();
    }
    return sts;
}

Box* fileFileno(BoxedFile* self) {
    assert(self->cls == file_cls);
    if (!self->f_fp)
        raiseExcHelper(IOError, "file is closed");

    return boxInt(fileno(self->f_fp));
}

Box* fileEnter(BoxedFile* self) {
    assert(self->cls == file_cls);
    return self;
}

Box* fileExit(BoxedFile* self, Box* exc_type, Box* exc_val, Box** args) {
    Box* exc_tb = args[0];
    assert(self->cls == file_cls);
    fileClose(self);
    return None;
}

// This differs very significantly from CPython:
Box* fileNew(BoxedClass* cls, Box* s, Box* m, Box** args) {
    BoxedInt* buffering = (BoxedInt*)args[0];

    assert(cls == file_cls);

    if (s->cls == unicode_cls)
        s = _PyUnicode_AsDefaultEncodedString(s, NULL);

    if (m->cls == unicode_cls)
        m = _PyUnicode_AsDefaultEncodedString(m, NULL);

    if (s->cls != str_cls) {
        raiseExcHelper(TypeError, "coercing to Unicode: need string of buffer, %s found", getTypeName(s));
    }
    if (m->cls != str_cls) {
        raiseExcHelper(TypeError, "coercing to Unicode: need string of buffer, %s found", getTypeName(m));
    }

    if (!PyInt_Check(buffering))
        raiseExcHelper(TypeError, "an integer is required");

    auto fn = static_cast<BoxedString*>(s);
    auto mode = static_cast<BoxedString*>(m);

    // all characters in python mode specifiers are valid in fopen calls except 'U'.  we strip it out
    // of the string we pass to fopen, but pass it along to the BoxedFile ctor.
    auto file_mode = std::unique_ptr<char[]>(new char[mode->size() + 1]);
    memmove(&file_mode[0], mode->data(), mode->size() + 1);
    _PyFile_SanitizeMode(&file_mode[0]);
    checkAndThrowCAPIException();

    FILE* f = fopen(fn->data(), &file_mode[0]);
    if (!f) {
        PyErr_SetFromErrnoWithFilename(IOError, fn->data());
        throwCAPIException();
        abort(); // unreachable;
    }

    auto file = new BoxedFile(f, fn->s(), PyString_AsString(m));
    PyFile_SetBufSize(file, buffering->n);
    return file;
}

static PyObject* file_readlines(BoxedFile* f, PyObject* args) noexcept {
    long sizehint = 0;
    PyObject* list = NULL;
    PyObject* line;
    char small_buffer[SMALLCHUNK];
    char* buffer = small_buffer;
    size_t buffersize = SMALLCHUNK;
    PyObject* big_buffer = NULL;
    size_t nfilled = 0;
    size_t nread;
    size_t totalread = 0;
    char* p, *q, *end;
    int err;
    int shortread = 0; /* bool, did the previous read come up short? */

    if (f->f_fp == NULL)
        return err_closed();
    if (!f->readable)
        return err_mode("reading");
    /* refuse to mix with f.next() */
    if (f->f_buf != NULL && (f->f_bufend - f->f_bufptr) > 0 && f->f_buf[0] != '\0')
        return err_iterbuffered();
    if (!PyArg_ParseTuple(args, "|l:readlines", &sizehint))
        return NULL;
    if ((list = PyList_New(0)) == NULL)
        return NULL;
    for (;;) {
        if (shortread)
            nread = 0;
        else {
            FILE_BEGIN_ALLOW_THREADS(f)
            errno = 0;
            nread = Py_UniversalNewlineFread(buffer + nfilled, buffersize - nfilled, f->f_fp, (PyObject*)f);
            FILE_END_ALLOW_THREADS(f)
            shortread = (nread < buffersize - nfilled);
        }
        if (nread == 0) {
            sizehint = 0;
            if (!ferror(f->f_fp))
                break;
            if (errno == EINTR) {
                if (PyErr_CheckSignals()) {
                    goto error;
                }
                clearerr(f->f_fp);
                shortread = 0;
                continue;
            }
            PyErr_SetFromErrno(PyExc_IOError);
            clearerr(f->f_fp);
            goto error;
        }
        totalread += nread;
        p = (char*)memchr(buffer + nfilled, '\n', nread);
        if (p == NULL) {
            /* Need a larger buffer to fit this line */
            nfilled += nread;
            buffersize *= 2;
            if (buffersize > PY_SSIZE_T_MAX) {
                PyErr_SetString(PyExc_OverflowError, "line is longer than a Python string can hold");
                goto error;
            }
            if (big_buffer == NULL) {
                /* Create the big buffer */
                big_buffer = PyString_FromStringAndSize(NULL, buffersize);
                if (big_buffer == NULL)
                    goto error;
                buffer = PyString_AS_STRING(big_buffer);
                memcpy(buffer, small_buffer, nfilled);
            } else {
                /* Grow the big buffer */
                if (_PyString_Resize(&big_buffer, buffersize) < 0)
                    goto error;
                buffer = PyString_AS_STRING(big_buffer);
            }
            continue;
        }
        end = buffer + nfilled + nread;
        q = buffer;
        do {
            /* Process complete lines */
            p++;
            line = PyString_FromStringAndSize(q, p - q);
            if (line == NULL)
                goto error;
            err = PyList_Append(list, line);
            Py_DECREF(line);
            if (err != 0)
                goto error;
            q = p;
            p = (char*)memchr(q, '\n', end - q);
        } while (p != NULL);
        /* Move the remaining incomplete line to the start */
        nfilled = end - q;
        memmove(buffer, q, nfilled);
        if (sizehint > 0)
            if (totalread >= (size_t)sizehint)
                break;
    }
    if (nfilled != 0) {
        /* Partial last line */
        line = PyString_FromStringAndSize(buffer, nfilled);
        if (line == NULL)
            goto error;
        if (sizehint > 0) {
            /* Need to complete the last line */
            PyObject* rest = get_line(f, 0);
            if (rest == NULL) {
                Py_DECREF(line);
                goto error;
            }
            PyString_Concat(&line, rest);
            Py_DECREF(rest);
            if (line == NULL)
                goto error;
        }
        err = PyList_Append(list, line);
        Py_DECREF(line);
        if (err != 0)
            goto error;
    }

cleanup:
    Py_XDECREF(big_buffer);
    return list;

error:
    Py_CLEAR(list);
    goto cleanup;
}

Box* fileIterNext(BoxedFile* s) {
    Box* rtn = fileReadline1(s);
    assert(!rtn || rtn->cls == str_cls);
    if (!rtn || ((BoxedString*)rtn)->s().empty())
        raiseExcHelper(StopIteration, (const char*)NULL);
    return rtn;
}

bool fileEof(BoxedFile* self) {
    char ch = fgetc(self->f_fp);
    ungetc(ch, self->f_fp);
    return feof(self->f_fp);
}

Box* fileIterHasNext(Box* s) {
    assert(s->cls == file_cls);
    BoxedFile* self = static_cast<BoxedFile*>(s);
    return boxBool(!fileEof(self));
}

extern "C" void PyFile_IncUseCount(PyFileObject* _f) noexcept {
    BoxedFile* f = reinterpret_cast<BoxedFile*>(_f);
    assert(f->cls == file_cls);
    f->unlocked_count++;
}

extern "C" void PyFile_DecUseCount(PyFileObject* _f) noexcept {
    BoxedFile* f = reinterpret_cast<BoxedFile*>(_f);
    assert(f->cls == file_cls);
    f->unlocked_count--;
    assert(f->unlocked_count >= 0);
}



extern "C" void PyFile_SetFP(PyObject* _f, FILE* fp) noexcept {
    assert(_f->cls == file_cls);
    BoxedFile* f = static_cast<BoxedFile*>(_f);
    assert(f->f_fp == NULL);
    f->f_fp = fp;
}

extern "C" PyObject* PyFile_FromFile(FILE* fp, const char* name, const char* mode, int (*close)(FILE*)) noexcept {
    return new BoxedFile(fp, name, mode, close);
}

extern "C" FILE* PyFile_AsFile(PyObject* f) noexcept {
    if (!f || !PyFile_Check(f))
        return NULL;

    return static_cast<BoxedFile*>(f)->f_fp;
}

extern "C" int PyFile_WriteObject(PyObject* v, PyObject* f, int flags) noexcept {
    PyObject* writer, *value, *args, *result;
    if (f == NULL) {
        PyErr_SetString(PyExc_TypeError, "writeobject with NULL file");
        return -1;
    } else if (PyFile_Check(f)) {
        BoxedFile* fobj = (BoxedFile*)f;
#ifdef Py_USING_UNICODE
        PyObject* enc = fobj->f_encoding;
        int result;
#endif
        if (fobj->f_fp == NULL) {
            err_closed();
            return -1;
        }
#ifdef Py_USING_UNICODE
        if ((flags & Py_PRINT_RAW) && PyUnicode_Check(v) && enc != Py_None) {
            char* cenc = PyString_AS_STRING(enc);
            const char* errors = fobj->f_errors == Py_None ? "strict" : PyString_AS_STRING(fobj->f_errors);
            value = PyUnicode_AsEncodedString(v, cenc, errors);
            if (value == NULL)
                return -1;
        } else {
            value = v;
            Py_INCREF(value);
        }
        // Pyston change:
        // result = file_PyObject_Print(value, fobj, flags);
        result = PyObject_Print(value, fobj->f_fp, flags);
        Py_DECREF(value);
        return result;
#else
        // Pyston change:
        // return file_PyObject_Print(v, fobj, flags);
        return PyObject_Print(v, fobj->f_fp, flags);
#endif
    }
    writer = PyObject_GetAttrString(f, "write");
    if (writer == NULL)
        return -1;
    if (flags & Py_PRINT_RAW) {
        if (PyUnicode_Check(v)) {
            value = v;
            Py_INCREF(value);
        } else
            value = PyObject_Str(v);
    } else
        value = PyObject_Repr(v);
    if (value == NULL) {
        Py_DECREF(writer);
        return -1;
    }
    args = PyTuple_Pack(1, value);
    if (args == NULL) {
        Py_DECREF(value);
        Py_DECREF(writer);
        return -1;
    }
    result = PyEval_CallObject(writer, args);
    Py_DECREF(args);
    Py_DECREF(value);
    Py_DECREF(writer);
    if (result == NULL)
        return -1;
    Py_DECREF(result);
    return 0;
}

extern "C" int PyFile_WriteString(const char* s, PyObject* f) noexcept {
    if (f == NULL) {
        /* Should be caused by a pre-existing error */
        if (!PyErr_Occurred())
            PyErr_SetString(PyExc_SystemError, "null file for PyFile_WriteString");
        return -1;
    } else if (PyFile_Check(f)) {
        BoxedFile* fobj = (BoxedFile*)f;
        FILE* fp = PyFile_AsFile(f);
        if (fp == NULL) {
            err_closed();
            return -1;
        }
        FILE_BEGIN_ALLOW_THREADS(fobj)
        fputs(s, fp);
        FILE_END_ALLOW_THREADS(fobj)
        return 0;
    } else if (!PyErr_Occurred()) {
        PyObject* v = PyString_FromString(s);
        int err;
        if (v == NULL)
            return -1;
        err = PyFile_WriteObject(v, f, Py_PRINT_RAW);
        Py_DECREF(v);
        return err;
    } else
        return -1;
}

extern "C" void PyFile_SetBufSize(PyObject* f, int bufsize) noexcept {
    assert(f->cls == file_cls);
    BoxedFile* file = (BoxedFile*)f;
    if (bufsize >= 0) {
        int type;
        switch (bufsize) {
            case 0:
                type = _IONBF;
                break;
#ifdef HAVE_SETVBUF
            case 1:
                type = _IOLBF;
                bufsize = BUFSIZ;
                break;
#endif
            default:
                type = _IOFBF;
#ifndef HAVE_SETVBUF
                bufsize = BUFSIZ;
#endif
                break;
        }
        fflush(file->f_fp);
        if (type == _IONBF) {
            PyMem_Free(file->f_setbuf);
            file->f_setbuf = NULL;
        } else {
            file->f_setbuf = (char*)PyMem_Realloc(file->f_setbuf, bufsize);
        }
#ifdef HAVE_SETVBUF
        setvbuf(file->f_fp, file->f_setbuf, type, bufsize);
#else  /* !HAVE_SETVBUF */
        setbuf(file->f_fp, file->f_setbuf);
#endif /* !HAVE_SETVBUF */
    }
}

/* Set the encoding used to output Unicode strings.
   Return 1 on success, 0 on failure. */

extern "C" int PyFile_SetEncoding(PyObject* f, const char* enc) noexcept {
    return PyFile_SetEncodingAndErrors(f, enc, NULL);
}

extern "C" int PyFile_SetEncodingAndErrors(PyObject* f, const char* enc, char* errors) noexcept {
    BoxedFile* file = static_cast<BoxedFile*>(f);
    PyObject* str, *oerrors;

    assert(PyFile_Check(f));
    str = PyString_FromString(enc);
    if (!str)
        return 0;
    if (errors) {
        oerrors = PyString_FromString(errors);
        if (!oerrors) {
            Py_DECREF(str);
            return 0;
        }
    } else {
        oerrors = Py_None;
        Py_INCREF(Py_None);
    }
    Py_DECREF(file->f_encoding);
    file->f_encoding = str;
    Py_DECREF(file->f_errors);
    file->f_errors = oerrors;
    return 1;
}

extern "C" int _PyFile_SanitizeMode(char* mode) noexcept {
    char* upos;
    size_t len = strlen(mode);

    if (!len) {
        PyErr_SetString(PyExc_ValueError, "empty mode string");
        return -1;
    }

    upos = strchr(mode, 'U');
    if (upos) {
        memmove(upos, upos + 1, len - (upos - mode)); /* incl null char */

        if (mode[0] == 'w' || mode[0] == 'a') {
            PyErr_Format(PyExc_ValueError, "universal newline "
                                           "mode can only be used with modes "
                                           "starting with 'r'");
            return -1;
        }

        if (mode[0] != 'r') {
            memmove(mode + 1, mode, strlen(mode) + 1);
            mode[0] = 'r';
        }

        if (!strchr(mode, 'b')) {
            memmove(mode + 2, mode + 1, strlen(mode));
            mode[1] = 'b';
        }
    } else if (mode[0] != 'r' && mode[0] != 'w' && mode[0] != 'a') {
        PyErr_Format(PyExc_ValueError, "mode string must begin with "
                                       "one of 'r', 'w', 'a' or 'U', not '%.200s'",
                     mode);
        return -1;
    }
#ifdef Py_VERIFY_WINNT
    /* additional checks on NT with visual studio 2005 and higher */
    if (!_PyVerify_Mode_WINNT(mode)) {
        PyErr_Format(PyExc_ValueError, "Invalid mode ('%.50s')", mode);
        return -1;
    }
#endif
    return 0;
}

extern "C" int PyObject_AsFileDescriptor(PyObject* o) noexcept {
    int fd;
    PyObject* meth;

    if (PyInt_Check(o)) {
        fd = _PyInt_AsInt(o);
    } else if (PyLong_Check(o)) {
        fd = _PyLong_AsInt(o);
    } else if ((meth = PyObject_GetAttrString(o, "fileno")) != NULL) {
        PyObject* fno = PyEval_CallObject(meth, NULL);
        Py_DECREF(meth);
        if (fno == NULL)
            return -1;

        if (PyInt_Check(fno)) {
            fd = _PyInt_AsInt(fno);
            Py_DECREF(fno);
        } else if (PyLong_Check(fno)) {
            fd = _PyLong_AsInt(fno);
            Py_DECREF(fno);
        } else {
            PyErr_SetString(PyExc_TypeError, "fileno() returned a non-integer");
            Py_DECREF(fno);
            return -1;
        }
    } else {
        PyErr_SetString(PyExc_TypeError, "argument must be an int, or have a fileno() method.");
        return -1;
    }

    if (fd < 0) {
        PyErr_Format(PyExc_ValueError, "file descriptor cannot be a negative integer (%i)", fd);
        return -1;
    }
    return fd;
}

extern "C" int PyFile_SoftSpace(PyObject* f, int newflag) noexcept {
    try {
        return softspace(f, newflag);
    } catch (ExcInfo e) {
        return 0;
    }
}

extern "C" PyObject* PyFile_GetLine(PyObject* f, int n) noexcept {
    PyObject* result;

    if (f == NULL) {
        PyErr_BadInternalCall();
        return NULL;
    }

    if (PyFile_Check(f)) {
        BoxedFile* fo = (BoxedFile*)f;
        if (fo->f_fp == NULL)
            return err_closed();
        if (!fo->readable)
            return err_mode("reading");
        /* refuse to mix with f.next() */
        if (fo->f_buf != NULL && (fo->f_bufend - fo->f_bufptr) > 0 && fo->f_buf[0] != '\0')
            return err_iterbuffered();
        result = get_line(fo, n);
    } else {
        PyObject* reader;
        PyObject* args;

        reader = PyObject_GetAttrString(f, "readline");
        if (reader == NULL)
            return NULL;
        if (n <= 0)
            args = PyTuple_New(0);
        else
            args = Py_BuildValue("(i)", n);
        if (args == NULL) {
            Py_DECREF(reader);
            return NULL;
        }
        result = PyEval_CallObject(reader, args);
        Py_DECREF(reader);
        Py_DECREF(args);
        if (result != NULL && !PyString_Check(result) && !PyUnicode_Check(result)) {
            Py_DECREF(result);
            result = NULL;
            PyErr_SetString(PyExc_TypeError, "object.readline() returned non-string");
        }
    }

    if (n < 0 && result != NULL && PyString_Check(result)) {
        char* s = PyString_AS_STRING(result);
        Py_ssize_t len = PyString_GET_SIZE(result);
        if (len == 0) {
            Py_DECREF(result);
            result = NULL;
            PyErr_SetString(PyExc_EOFError, "EOF when reading a line");
        } else if (s[len - 1] == '\n') {
            if (/*result->ob_refcnt*/ 2 == 1) {
                if (_PyString_Resize(&result, len - 1))
                    return NULL;
            } else {
                PyObject* v;
                v = PyString_FromStringAndSize(s, len - 1);
                Py_DECREF(result);
                result = v;
            }
        }
    }
#ifdef Py_USING_UNICODE
    if (n < 0 && result != NULL && PyUnicode_Check(result)) {
        Py_UNICODE* s = PyUnicode_AS_UNICODE(result);
        Py_ssize_t len = PyUnicode_GET_SIZE(result);
        if (len == 0) {
            Py_DECREF(result);
            result = NULL;
            PyErr_SetString(PyExc_EOFError, "EOF when reading a line");
        } else if (s[len - 1] == '\n') {
            if (/*result->ob_refcnt*/ 2 == 1)
                PyUnicode_Resize(&result, len - 1);
            else {
                PyObject* v;
                v = PyUnicode_FromUnicode(s, len - 1);
                Py_DECREF(result);
                result = v;
            }
        }
    }
#endif
    return result;
}
/*
** Py_UniversalNewlineFread is an fread variation that understands
** all of \r, \n and \r\n conventions.
** The stream should be opened in binary mode.
** fobj must be a PyFileObject. In this case there
** is no readahead but in stead a flag is used to skip a following
** \n on the next read. Also, if the file is open in binary mode
** the whole conversion is skipped. Finally, the routine keeps track of
** the different types of newlines seen.
*/
extern "C" size_t Py_UniversalNewlineFread(char* buf, size_t n, FILE* stream, PyObject* fobj) noexcept {
    char* dst = buf;
    BoxedFile* f = (BoxedFile*)fobj;
    int newlinetypes, skipnextlf;

    assert(buf != NULL);
    assert(stream != NULL);

    if (!fobj || !PyFile_Check(fobj)) {
        errno = ENXIO; /* What can you do... */
        return 0;
    }
    if (!f->f_univ_newline)
        return fread(buf, 1, n, stream);
    newlinetypes = f->f_newlinetypes;
    skipnextlf = f->f_skipnextlf;
    /* Invariant:  n is the number of bytes remaining to be filled
     * in the buffer.
     */
    while (n) {
        size_t nread;
        int shortread;
        char* src = dst;

        nread = fread(dst, 1, n, stream);
        assert(nread <= n);
        if (nread == 0)
            break;

        n -= nread;         /* assuming 1 byte out for each in; will adjust */
        shortread = n != 0; /* true iff EOF or error */
        while (nread--) {
            char c = *src++;
            if (c == '\r') {
                /* Save as LF and set flag to skip next LF. */
                *dst++ = '\n';
                skipnextlf = 1;
            } else if (skipnextlf && c == '\n') {
                /* Skip LF, and remember we saw CR LF. */
                skipnextlf = 0;
                newlinetypes |= NEWLINE_CRLF;
                ++n;
            } else {
                /* Normal char to be stored in buffer.  Also
                 * update the newlinetypes flag if either this
                 * is an LF or the previous char was a CR.
                 */
                if (c == '\n')
                    newlinetypes |= NEWLINE_LF;
                else if (skipnextlf)
                    newlinetypes |= NEWLINE_CR;
                *dst++ = c;
                skipnextlf = 0;
            }
        }
        if (shortread) {
            /* If this is EOF, update type flags. */
            if (skipnextlf && feof(stream))
                newlinetypes |= NEWLINE_CR;
            break;
        }
    }
    f->f_newlinetypes = newlinetypes;
    f->f_skipnextlf = skipnextlf;
    return dst - buf;
}

static PyObject* file_isatty(BoxedFile* f) noexcept {
    long res;
    if (f->f_fp == NULL)
        return err_closed();
    FILE_BEGIN_ALLOW_THREADS(f)
    res = isatty((int)fileno(f->f_fp));
    FILE_END_ALLOW_THREADS(f)
    return PyBool_FromLong(res);
}

static PyObject* get_closed(BoxedFile* f, void* closure) noexcept {
    return PyBool_FromLong((long)(f->f_fp == 0));
}

static PyObject* file_truncate(BoxedFile* f, PyObject* args) {
    Py_off_t newsize;
    PyObject* newsizeobj = NULL;
    Py_off_t initialpos;
    int ret;

    if (f->f_fp == NULL)
        return err_closed();
    if (!f->writable)
        return err_mode("writing");
    if (!PyArg_UnpackTuple(args, "truncate", 0, 1, &newsizeobj))
        return NULL;

    /* Get current file position.  If the file happens to be open for
     * update and the last operation was an input operation, C doesn't
     * define what the later fflush() will do, but we promise truncate()
     * won't change the current position (and fflush() *does* change it
     * then at least on Windows).  The easiest thing is to capture
     * current pos now and seek back to it at the end.
     */
    FILE_BEGIN_ALLOW_THREADS(f)
    errno = 0;
    initialpos = _portable_ftell(f->f_fp);
    FILE_END_ALLOW_THREADS(f)
    if (initialpos == -1)
        goto onioerror;

    /* Set newsize to current postion if newsizeobj NULL, else to the
     * specified value.
     */
    if (newsizeobj != NULL) {
#if !defined(HAVE_LARGEFILE_SUPPORT)
        newsize = PyInt_AsLong(newsizeobj);
#else
        newsize = PyLong_Check(newsizeobj) ? PyLong_AsLongLong(newsizeobj) : PyInt_AsLong(newsizeobj);
#endif
        if (PyErr_Occurred())
            return NULL;
    } else /* default to current position */
        newsize = initialpos;

    /* Flush the stream.  We're mixing stream-level I/O with lower-level
     * I/O, and a flush may be necessary to synch both platform views
     * of the current file state.
     */
    FILE_BEGIN_ALLOW_THREADS(f)
    errno = 0;
    ret = fflush(f->f_fp);
    FILE_END_ALLOW_THREADS(f)
    if (ret != 0)
        goto onioerror;

#ifdef MS_WINDOWS
    /* MS _chsize doesn't work if newsize doesn't fit in 32 bits,
       so don't even try using it. */
    {
        HANDLE hFile;

        /* Have to move current pos to desired endpoint on Windows. */
        FILE_BEGIN_ALLOW_THREADS(f)
        errno = 0;
        ret = _portable_fseek(f->f_fp, newsize, SEEK_SET) != 0;
        FILE_END_ALLOW_THREADS(f)
        if (ret)
            goto onioerror;

        /* Truncate.  Note that this may grow the file! */
        FILE_BEGIN_ALLOW_THREADS(f)
        errno = 0;
        hFile = (HANDLE)_get_osfhandle(fileno(f->f_fp));
        ret = hFile == (HANDLE)-1;
        if (ret == 0) {
            ret = SetEndOfFile(hFile) == 0;
            if (ret)
                errno = EACCES;
        }
        FILE_END_ALLOW_THREADS(f)
        if (ret)
            goto onioerror;
    }
#else
    FILE_BEGIN_ALLOW_THREADS(f)
    errno = 0;
    ret = ftruncate(fileno(f->f_fp), newsize);
    FILE_END_ALLOW_THREADS(f)
    if (ret != 0)
        goto onioerror;
#endif /* !MS_WINDOWS */

    /* Restore original file position. */
    FILE_BEGIN_ALLOW_THREADS(f)
    errno = 0;
    ret = _portable_fseek(f->f_fp, initialpos, SEEK_SET) != 0;
    FILE_END_ALLOW_THREADS(f)
    if (ret)
        goto onioerror;

    Py_INCREF(Py_None);
    return Py_None;

onioerror:
    PyErr_SetFromErrno(PyExc_IOError);
    clearerr(f->f_fp);
    return NULL;
}


PyDoc_STRVAR(seek_doc, "seek(offset[, whence]) -> None.  Move to new file position.\n"
                       "\n"
                       "Argument offset is a byte count.  Optional argument whence defaults to\n"
                       "0 (offset from start of file, offset should be >= 0); other values are 1\n"
                       "(move relative to current position, positive or negative), and 2 (move\n"
                       "relative to end of file, usually negative, although many platforms allow\n"
                       "seeking beyond the end of a file).  If the file is opened in text mode,\n"
                       "only offsets returned by tell() are legal.  Use of other offsets causes\n"
                       "undefined behavior."
                       "\n"
                       "Note that not all file objects are seekable.");

PyDoc_STRVAR(truncate_doc, "truncate([size]) -> None.  Truncate the file to at most size bytes.\n"
                           "\n"
                           "Size defaults to the current file position, as returned by tell().");

PyDoc_STRVAR(readlines_doc, "readlines([size]) -> list of strings, each a line from the file.\n"
                            "\n"
                            "Call readline() repeatedly and return a list of the lines so read.\n"
                            "The optional size argument, if given, is an approximate bound on the\n"
                            "total number of bytes in the lines returned.");

PyDoc_STRVAR(isatty_doc, "isatty() -> true or false.  True if the file is connected to a tty device.");

static PyMethodDef file_methods[] = {
    { "seek", (PyCFunction)file_seek, METH_VARARGS, seek_doc },
    { "truncate", (PyCFunction)file_truncate, METH_VARARGS, truncate_doc },
    { "readlines", (PyCFunction)file_readlines, METH_VARARGS, readlines_doc },
    { "writelines", (PyCFunction)file_writelines, METH_O, NULL },
    { "isatty", (PyCFunction)file_isatty, METH_NOARGS, isatty_doc },
};

static PyGetSetDef file_getsetlist[] = {
    { "closed", (getter)get_closed, NULL, "True if the file is closed", NULL },
};

void fileDestructor(Box* b) {
    assert(isSubclass(b->cls, file_cls));
    BoxedFile* self = static_cast<BoxedFile*>(b);

    if (self->f_fp && self->f_close)
        self->f_close(self->f_fp);
    self->f_fp = NULL;
}

void BoxedFile::gcHandler(GCVisitor* v, Box* b) {
    Box::gcHandler(v, b);

    assert(isSubclass(b->cls, file_cls));
    BoxedFile* f = static_cast<BoxedFile*>(b);

    v->visit(&f->f_name);
    v->visit(&f->f_mode);
    v->visit(&f->f_encoding);
    v->visit(&f->f_errors);
    v->visit(&f->f_setbuf);
}

void setupFile() {
    file_cls->tp_dealloc = fileDestructor;
    file_cls->has_safe_tp_dealloc = true;

    file_cls->giveAttr("read", new BoxedFunction(boxRTFunction((void*)fileRead, STR, 2, false, false), { boxInt(-1) }));

    CLFunction* readline = boxRTFunction((void*)fileReadline1, STR, 1);
    file_cls->giveAttr("readline", new BoxedFunction(readline));

    file_cls->giveAttr("flush", new BoxedFunction(boxRTFunction((void*)fileFlush, NONE, 1)));
    file_cls->giveAttr("write", new BoxedFunction(boxRTFunction((void*)fileWrite, NONE, 2)));
    file_cls->giveAttr("close", new BoxedFunction(boxRTFunction((void*)fileClose, UNKNOWN, 1)));
    file_cls->giveAttr("fileno", new BoxedFunction(boxRTFunction((void*)fileFileno, BOXED_INT, 1)));

    file_cls->giveAttr("__repr__", new BoxedFunction(boxRTFunction((void*)fileRepr, STR, 1)));

    file_cls->giveAttr("__enter__", new BoxedFunction(boxRTFunction((void*)fileEnter, typeFromClass(file_cls), 1)));
    file_cls->giveAttr("__exit__", new BoxedFunction(boxRTFunction((void*)fileExit, UNKNOWN, 4)));

    file_cls->giveAttr("__iter__", file_cls->getattr(internStringMortal("__enter__")));
    file_cls->giveAttr("__hasnext__", new BoxedFunction(boxRTFunction((void*)fileIterHasNext, BOXED_BOOL, 1)));
    file_cls->giveAttr("next", new BoxedFunction(boxRTFunction((void*)fileIterNext, STR, 1)));

    file_cls->giveAttr("tell", new BoxedFunction(boxRTFunction((void*)fileTell, UNKNOWN, 1)));
    file_cls->giveAttr("softspace",
                       new BoxedMemberDescriptor(BoxedMemberDescriptor::INT, offsetof(BoxedFile, f_softspace), false));
    file_cls->giveAttr("name",
                       new BoxedMemberDescriptor(BoxedMemberDescriptor::OBJECT, offsetof(BoxedFile, f_name), true));
    file_cls->giveAttr("mode",
                       new BoxedMemberDescriptor(BoxedMemberDescriptor::OBJECT, offsetof(BoxedFile, f_mode), true));

    file_cls->giveAttr("__new__", new BoxedFunction(boxRTFunction((void*)fileNew, UNKNOWN, 4, false, false),
                                                    { boxString("r"), boxInt(-1) }));

    for (auto& md : file_methods) {
        file_cls->giveAttr(md.ml_name, new BoxedMethodDescriptor(&md, file_cls));
    }

    for (auto& getset : file_getsetlist) {
        file_cls->giveAttr(getset.name, new (capi_getset_cls) BoxedGetsetDescriptor(
                                            getset.get, (void (*)(Box*, Box*, void*))getset.set, getset.closure));
    }

    file_cls->freeze();
}

void teardownFile() {
}
}
