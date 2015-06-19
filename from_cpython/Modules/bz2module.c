/*

python-bz2 - python bz2 library interface

Copyright (c) 2002  Gustavo Niemeyer <niemeyer@conectiva.com>
Copyright (c) 2002  Python Software Foundation; All Rights Reserved

*/

#include "Python.h"
#include <stdio.h>
#include <bzlib.h>
#include "structmember.h"

#ifdef WITH_THREAD
#include "pythread.h"
#endif

static char __author__[] =
"The bz2 python module was written by:\n\
\n\
    Gustavo Niemeyer <niemeyer@conectiva.com>\n\
";

/* Our very own off_t-like type, 64-bit if possible */
/* copied from Objects/fileobject.c */
#if !defined(HAVE_LARGEFILE_SUPPORT)
typedef off_t Py_off_t;
#elif SIZEOF_OFF_T >= 8
typedef off_t Py_off_t;
#elif SIZEOF_FPOS_T >= 8
typedef fpos_t Py_off_t;
#else
#error "Large file support, but neither off_t nor fpos_t is large enough."
#endif

#define BUF(v) PyString_AS_STRING((PyStringObject *)v)

#define MODE_CLOSED   0
#define MODE_READ     1
#define MODE_READ_EOF 2
#define MODE_WRITE    3


#ifndef BZ_CONFIG_ERROR

#define BZ2_bzRead bzRead
#define BZ2_bzReadOpen bzReadOpen
#define BZ2_bzReadClose bzReadClose
#define BZ2_bzWrite bzWrite
#define BZ2_bzWriteOpen bzWriteOpen
#define BZ2_bzWriteClose bzWriteClose
#define BZ2_bzCompress bzCompress
#define BZ2_bzCompressInit bzCompressInit
#define BZ2_bzCompressEnd bzCompressEnd
#define BZ2_bzDecompress bzDecompress
#define BZ2_bzDecompressInit bzDecompressInit
#define BZ2_bzDecompressEnd bzDecompressEnd

#endif /* ! BZ_CONFIG_ERROR */


#ifdef WITH_THREAD
#define ACQUIRE_LOCK(obj) do { \
    if (!PyThread_acquire_lock(obj->lock, 0)) { \
        Py_BEGIN_ALLOW_THREADS \
        PyThread_acquire_lock(obj->lock, 1); \
        Py_END_ALLOW_THREADS \
    } } while(0)
#define RELEASE_LOCK(obj) PyThread_release_lock(obj->lock)
#else
#define ACQUIRE_LOCK(obj)
#define RELEASE_LOCK(obj)
#endif

#define MIN(X, Y) (((X) < (Y)) ? (X) : (Y))

/* Bits in f_newlinetypes */
#define NEWLINE_UNKNOWN 0       /* No newline seen, yet */
#define NEWLINE_CR 1            /* \r newline seen */
#define NEWLINE_LF 2            /* \n newline seen */
#define NEWLINE_CRLF 4          /* \r\n newline seen */

/* ===================================================================== */
/* Structure definitions. */

typedef struct {
    PyObject_HEAD
    PyObject *file;

    char* f_buf;                /* Allocated readahead buffer */
    char* f_bufend;             /* Points after last occupied position */
    char* f_bufptr;             /* Current buffer position */

    int f_softspace;            /* Flag used by 'print' command */

    int f_univ_newline;         /* Handle any newline convention */
    int f_newlinetypes;         /* Types of newlines seen */
    int f_skipnextlf;           /* Skip next \n */

    BZFILE *fp;
    int mode;
    Py_off_t pos;
    Py_off_t size;
#ifdef WITH_THREAD
    PyThread_type_lock lock;
#endif
} BZ2FileObject;

typedef struct {
    PyObject_HEAD
    bz_stream bzs;
    int running;
#ifdef WITH_THREAD
    PyThread_type_lock lock;
#endif
} BZ2CompObject;

typedef struct {
    PyObject_HEAD
    bz_stream bzs;
    int running;
    PyObject *unused_data;
#ifdef WITH_THREAD
    PyThread_type_lock lock;
#endif
} BZ2DecompObject;

/* ===================================================================== */
/* Utility functions. */

/* Refuse regular I/O if there's data in the iteration-buffer.
 * Mixing them would cause data to arrive out of order, as the read*
 * methods don't use the iteration buffer. */
static int
check_iterbuffered(BZ2FileObject *f)
{
    if (f->f_buf != NULL &&
        (f->f_bufend - f->f_bufptr) > 0 &&
        f->f_buf[0] != '\0') {
        PyErr_SetString(PyExc_ValueError,
            "Mixing iteration and read methods would lose data");
        return -1;
    }
    return 0;
}

static int
Util_CatchBZ2Error(int bzerror)
{
    int ret = 0;
    switch(bzerror) {
        case BZ_OK:
        case BZ_STREAM_END:
            break;

#ifdef BZ_CONFIG_ERROR
        case BZ_CONFIG_ERROR:
            PyErr_SetString(PyExc_SystemError,
                            "the bz2 library was not compiled "
                            "correctly");
            ret = 1;
            break;
#endif

        case BZ_PARAM_ERROR:
            PyErr_SetString(PyExc_ValueError,
                            "the bz2 library has received wrong "
                            "parameters");
            ret = 1;
            break;

        case BZ_MEM_ERROR:
            PyErr_NoMemory();
            ret = 1;
            break;

        case BZ_DATA_ERROR:
        case BZ_DATA_ERROR_MAGIC:
            PyErr_SetString(PyExc_IOError, "invalid data stream");
            ret = 1;
            break;

        case BZ_IO_ERROR:
            PyErr_SetString(PyExc_IOError, "unknown IO error");
            ret = 1;
            break;

        case BZ_UNEXPECTED_EOF:
            PyErr_SetString(PyExc_EOFError,
                            "compressed file ended before the "
                            "logical end-of-stream was detected");
            ret = 1;
            break;

        case BZ_SEQUENCE_ERROR:
            PyErr_SetString(PyExc_RuntimeError,
                            "wrong sequence of bz2 library "
                            "commands used");
            ret = 1;
            break;
    }
    return ret;
}

#if BUFSIZ < 8192
#define SMALLCHUNK 8192
#else
#define SMALLCHUNK BUFSIZ
#endif

/* This is a hacked version of Python's fileobject.c:new_buffersize(). */
static size_t
Util_NewBufferSize(size_t currentsize)
{
    /* Expand the buffer by an amount proportional to the current size,
       giving us amortized linear-time behavior. Use a less-than-double
       growth factor to avoid excessive allocation. */
    return currentsize + (currentsize >> 3) + 6;
}

static int
Util_GrowBuffer(PyObject **buf)
{
    size_t size = PyString_GET_SIZE(*buf);
    size_t new_size = Util_NewBufferSize(size);
    if (new_size > size) {
        return _PyString_Resize(buf, new_size);
    } else {  /* overflow */
        PyErr_SetString(PyExc_OverflowError,
                        "Unable to allocate buffer - output too large");
        return -1;
    }
}

/* This is a hacked version of Python's fileobject.c:get_line(). */
static PyObject *
Util_GetLine(BZ2FileObject *f, int n)
{
    char c;
    char *buf, *end;
    size_t total_v_size;        /* total # of slots in buffer */
    size_t used_v_size;         /* # used slots in buffer */
    size_t increment;       /* amount to increment the buffer */
    PyObject *v;
    int bzerror;
    int bytes_read;
    int newlinetypes = f->f_newlinetypes;
    int skipnextlf = f->f_skipnextlf;
    int univ_newline = f->f_univ_newline;

    total_v_size = n > 0 ? n : 100;
    v = PyString_FromStringAndSize((char *)NULL, total_v_size);
    if (v == NULL)
        return NULL;

    buf = BUF(v);
    end = buf + total_v_size;

    for (;;) {
        Py_BEGIN_ALLOW_THREADS
        while (buf != end) {
            bytes_read = BZ2_bzRead(&bzerror, f->fp, &c, 1);
            f->pos++;
            if (bytes_read == 0) break;
            if (univ_newline) {
                if (skipnextlf) {
                    skipnextlf = 0;
                    if (c == '\n') {
                        /* Seeing a \n here with skipnextlf true means we
                         * saw a \r before.
                         */
                        newlinetypes |= NEWLINE_CRLF;
                        if (bzerror != BZ_OK) break;
                        bytes_read = BZ2_bzRead(&bzerror, f->fp, &c, 1);
                        f->pos++;
                        if (bytes_read == 0) break;
                    } else {
                        newlinetypes |= NEWLINE_CR;
                    }
                }
                if (c == '\r') {
                    skipnextlf = 1;
                    c = '\n';
                } else if (c == '\n')
                    newlinetypes |= NEWLINE_LF;
            }
            *buf++ = c;
            if (bzerror != BZ_OK || c == '\n') break;
        }
        if (univ_newline && bzerror == BZ_STREAM_END && skipnextlf)
            newlinetypes |= NEWLINE_CR;
        Py_END_ALLOW_THREADS
        f->f_newlinetypes = newlinetypes;
        f->f_skipnextlf = skipnextlf;
        if (bzerror == BZ_STREAM_END) {
            f->size = f->pos;
            f->mode = MODE_READ_EOF;
            break;
        } else if (bzerror != BZ_OK) {
            Util_CatchBZ2Error(bzerror);
            Py_DECREF(v);
            return NULL;
        }
        if (c == '\n')
            break;
        /* Must be because buf == end */
        if (n > 0)
            break;
        used_v_size = total_v_size;
        increment = total_v_size >> 2; /* mild exponential growth */
        total_v_size += increment;
        if (total_v_size > INT_MAX) {
            PyErr_SetString(PyExc_OverflowError,
                "line is longer than a Python string can hold");
            Py_DECREF(v);
            return NULL;
        }
        if (_PyString_Resize(&v, total_v_size) < 0)
            return NULL;
        buf = BUF(v) + used_v_size;
        end = BUF(v) + total_v_size;
    }

    used_v_size = buf - BUF(v);
    if (used_v_size != total_v_size)
        _PyString_Resize(&v, used_v_size);
    return v;
}

/* This is a hacked version of Python's
 * fileobject.c:Py_UniversalNewlineFread(). */
size_t
Util_UnivNewlineRead(int *bzerror, BZFILE *stream,
                     char* buf, size_t n, BZ2FileObject *f)
{
    char *dst = buf;
    int newlinetypes, skipnextlf;

    assert(buf != NULL);
    assert(stream != NULL);

    if (!f->f_univ_newline)
        return BZ2_bzRead(bzerror, stream, buf, n);

    newlinetypes = f->f_newlinetypes;
    skipnextlf = f->f_skipnextlf;

    /* Invariant:  n is the number of bytes remaining to be filled
     * in the buffer.
     */
    while (n) {
        size_t nread;
        int shortread;
        char *src = dst;

        nread = BZ2_bzRead(bzerror, stream, dst, n);
        assert(nread <= n);
        n -= nread; /* assuming 1 byte out for each in; will adjust */
        shortread = n != 0;             /* true iff EOF or error */
        while (nread--) {
            char c = *src++;
            if (c == '\r') {
                /* Save as LF and set flag to skip next LF. */
                *dst++ = '\n';
                skipnextlf = 1;
            }
            else if (skipnextlf && c == '\n') {
                /* Skip LF, and remember we saw CR LF. */
                skipnextlf = 0;
                newlinetypes |= NEWLINE_CRLF;
                ++n;
            }
            else {
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
            if (skipnextlf && *bzerror == BZ_STREAM_END)
                newlinetypes |= NEWLINE_CR;
            break;
        }
    }
    f->f_newlinetypes = newlinetypes;
    f->f_skipnextlf = skipnextlf;
    return dst - buf;
}

/* This is a hacked version of Python's fileobject.c:drop_readahead(). */
static void
Util_DropReadAhead(BZ2FileObject *f)
{
    if (f->f_buf != NULL) {
        PyMem_Free(f->f_buf);
        f->f_buf = NULL;
    }
}

/* This is a hacked version of Python's fileobject.c:readahead(). */
static int
Util_ReadAhead(BZ2FileObject *f, int bufsize)
{
    int chunksize;
    int bzerror;

    if (f->f_buf != NULL) {
        if((f->f_bufend - f->f_bufptr) >= 1)
            return 0;
        else
            Util_DropReadAhead(f);
    }
    if (f->mode == MODE_READ_EOF) {
        f->f_bufptr = f->f_buf;
        f->f_bufend = f->f_buf;
        return 0;
    }
    if ((f->f_buf = PyMem_Malloc(bufsize)) == NULL) {
        PyErr_NoMemory();
        return -1;
    }
    Py_BEGIN_ALLOW_THREADS
    chunksize = Util_UnivNewlineRead(&bzerror, f->fp, f->f_buf,
                                     bufsize, f);
    Py_END_ALLOW_THREADS
    f->pos += chunksize;
    if (bzerror == BZ_STREAM_END) {
        f->size = f->pos;
        f->mode = MODE_READ_EOF;
    } else if (bzerror != BZ_OK) {
        Util_CatchBZ2Error(bzerror);
        Util_DropReadAhead(f);
        return -1;
    }
    f->f_bufptr = f->f_buf;
    f->f_bufend = f->f_buf + chunksize;
    return 0;
}

/* This is a hacked version of Python's
 * fileobject.c:readahead_get_line_skip(). */
static PyStringObject *
Util_ReadAheadGetLineSkip(BZ2FileObject *f, int skip, int bufsize)
{
    PyStringObject* s;
    char *bufptr;
    char *buf;
    int len;

    if (f->f_buf == NULL)
        if (Util_ReadAhead(f, bufsize) < 0)
            return NULL;

    len = f->f_bufend - f->f_bufptr;
    if (len == 0)
        return (PyStringObject *)
            PyString_FromStringAndSize(NULL, skip);
    bufptr = memchr(f->f_bufptr, '\n', len);
    if (bufptr != NULL) {
        bufptr++;                               /* Count the '\n' */
        len = bufptr - f->f_bufptr;
        s = (PyStringObject *)
            PyString_FromStringAndSize(NULL, skip+len);
        if (s == NULL)
            return NULL;
        memcpy(PyString_AS_STRING(s)+skip, f->f_bufptr, len);
        f->f_bufptr = bufptr;
        if (bufptr == f->f_bufend)
            Util_DropReadAhead(f);
    } else {
        bufptr = f->f_bufptr;
        buf = f->f_buf;
        f->f_buf = NULL;                /* Force new readahead buffer */
        s = Util_ReadAheadGetLineSkip(f, skip+len,
                                      bufsize + (bufsize>>2));
        if (s == NULL) {
            PyMem_Free(buf);
            return NULL;
        }
        memcpy(PyString_AS_STRING(s)+skip, bufptr, len);
        PyMem_Free(buf);
    }
    return s;
}

/* ===================================================================== */
/* Methods of BZ2File. */

PyDoc_STRVAR(BZ2File_read__doc__,
"read([size]) -> string\n\
\n\
Read at most size uncompressed bytes, returned as a string. If the size\n\
argument is negative or omitted, read until EOF is reached.\n\
");

/* This is a hacked version of Python's fileobject.c:file_read(). */
static PyObject *
BZ2File_read(BZ2FileObject *self, PyObject *args)
{
    long bytesrequested = -1;
    size_t bytesread, buffersize, chunksize;
    int bzerror;
    PyObject *ret = NULL;

    if (!PyArg_ParseTuple(args, "|l:read", &bytesrequested))
        return NULL;

    ACQUIRE_LOCK(self);
    switch (self->mode) {
        case MODE_READ:
            break;
        case MODE_READ_EOF:
            ret = PyString_FromString("");
            goto cleanup;
        case MODE_CLOSED:
            PyErr_SetString(PyExc_ValueError,
                            "I/O operation on closed file");
            goto cleanup;
        default:
            PyErr_SetString(PyExc_IOError,
                            "file is not ready for reading");
            goto cleanup;
    }

    /* refuse to mix with f.next() */
    if (check_iterbuffered(self))
        goto cleanup;

    if (bytesrequested < 0)
        buffersize = Util_NewBufferSize((size_t)0);
    else
        buffersize = bytesrequested;
    if (buffersize > INT_MAX) {
        PyErr_SetString(PyExc_OverflowError,
                        "requested number of bytes is "
                        "more than a Python string can hold");
        goto cleanup;
    }
    ret = PyString_FromStringAndSize((char *)NULL, buffersize);
    if (ret == NULL)
        goto cleanup;
    bytesread = 0;

    for (;;) {
        Py_BEGIN_ALLOW_THREADS
        chunksize = Util_UnivNewlineRead(&bzerror, self->fp,
                                         BUF(ret)+bytesread,
                                         buffersize-bytesread,
                                         self);
        self->pos += chunksize;
        Py_END_ALLOW_THREADS
        bytesread += chunksize;
        if (bzerror == BZ_STREAM_END) {
            self->size = self->pos;
            self->mode = MODE_READ_EOF;
            break;
        } else if (bzerror != BZ_OK) {
            Util_CatchBZ2Error(bzerror);
            Py_DECREF(ret);
            ret = NULL;
            goto cleanup;
        }
        if (bytesrequested < 0) {
            buffersize = Util_NewBufferSize(buffersize);
            if (_PyString_Resize(&ret, buffersize) < 0)
                goto cleanup;
        } else {
            break;
        }
    }
    if (bytesread != buffersize)
        _PyString_Resize(&ret, bytesread);

cleanup:
    RELEASE_LOCK(self);
    return ret;
}

PyDoc_STRVAR(BZ2File_readline__doc__,
"readline([size]) -> string\n\
\n\
Return the next line from the file, as a string, retaining newline.\n\
A non-negative size argument will limit the maximum number of bytes to\n\
return (an incomplete line may be returned then). Return an empty\n\
string at EOF.\n\
");

static PyObject *
BZ2File_readline(BZ2FileObject *self, PyObject *args)
{
    PyObject *ret = NULL;
    int sizehint = -1;

    if (!PyArg_ParseTuple(args, "|i:readline", &sizehint))
        return NULL;

    ACQUIRE_LOCK(self);
    switch (self->mode) {
        case MODE_READ:
            break;
        case MODE_READ_EOF:
            ret = PyString_FromString("");
            goto cleanup;
        case MODE_CLOSED:
            PyErr_SetString(PyExc_ValueError,
                            "I/O operation on closed file");
            goto cleanup;
        default:
            PyErr_SetString(PyExc_IOError,
                            "file is not ready for reading");
            goto cleanup;
    }

    /* refuse to mix with f.next() */
    if (check_iterbuffered(self))
        goto cleanup;

    if (sizehint == 0)
        ret = PyString_FromString("");
    else
        ret = Util_GetLine(self, (sizehint < 0) ? 0 : sizehint);

cleanup:
    RELEASE_LOCK(self);
    return ret;
}

PyDoc_STRVAR(BZ2File_readlines__doc__,
"readlines([size]) -> list\n\
\n\
Call readline() repeatedly and return a list of lines read.\n\
The optional size argument, if given, is an approximate bound on the\n\
total number of bytes in the lines returned.\n\
");

/* This is a hacked version of Python's fileobject.c:file_readlines(). */
static PyObject *
BZ2File_readlines(BZ2FileObject *self, PyObject *args)
{
    long sizehint = 0;
    PyObject *list = NULL;
    PyObject *line;
    char small_buffer[SMALLCHUNK];
    char *buffer = small_buffer;
    size_t buffersize = SMALLCHUNK;
    PyObject *big_buffer = NULL;
    size_t nfilled = 0;
    size_t nread;
    size_t totalread = 0;
    char *p, *q, *end;
    int err;
    int shortread = 0;
    int bzerror;

    if (!PyArg_ParseTuple(args, "|l:readlines", &sizehint))
        return NULL;

    ACQUIRE_LOCK(self);
    switch (self->mode) {
        case MODE_READ:
            break;
        case MODE_READ_EOF:
            list = PyList_New(0);
            goto cleanup;
        case MODE_CLOSED:
            PyErr_SetString(PyExc_ValueError,
                            "I/O operation on closed file");
            goto cleanup;
        default:
            PyErr_SetString(PyExc_IOError,
                            "file is not ready for reading");
            goto cleanup;
    }

    /* refuse to mix with f.next() */
    if (check_iterbuffered(self))
        goto cleanup;

    if ((list = PyList_New(0)) == NULL)
        goto cleanup;

    for (;;) {
        Py_BEGIN_ALLOW_THREADS
        nread = Util_UnivNewlineRead(&bzerror, self->fp,
                                     buffer+nfilled,
                                     buffersize-nfilled, self);
        self->pos += nread;
        Py_END_ALLOW_THREADS
        if (bzerror == BZ_STREAM_END) {
            self->size = self->pos;
            self->mode = MODE_READ_EOF;
            if (nread == 0) {
                sizehint = 0;
                break;
            }
            shortread = 1;
        } else if (bzerror != BZ_OK) {
            Util_CatchBZ2Error(bzerror);
          error:
            Py_DECREF(list);
            list = NULL;
            goto cleanup;
        }
        totalread += nread;
        p = memchr(buffer+nfilled, '\n', nread);
        if (!shortread && p == NULL) {
            /* Need a larger buffer to fit this line */
            nfilled += nread;
            buffersize *= 2;
            if (buffersize > INT_MAX) {
                PyErr_SetString(PyExc_OverflowError,
                "line is longer than a Python string can hold");
                goto error;
            }
            if (big_buffer == NULL) {
                /* Create the big buffer */
                big_buffer = PyString_FromStringAndSize(
                    NULL, buffersize);
                if (big_buffer == NULL)
                    goto error;
                buffer = PyString_AS_STRING(big_buffer);
                memcpy(buffer, small_buffer, nfilled);
            }
            else {
                /* Grow the big buffer */
                if (_PyString_Resize(&big_buffer, buffersize))
                    goto error;
                buffer = PyString_AS_STRING(big_buffer);
            }
            continue;
        }
        end = buffer+nfilled+nread;
        q = buffer;
        while (p != NULL) {
            /* Process complete lines */
            p++;
            line = PyString_FromStringAndSize(q, p-q);
            if (line == NULL)
                goto error;
            err = PyList_Append(list, line);
            Py_DECREF(line);
            if (err != 0)
                goto error;
            q = p;
            p = memchr(q, '\n', end-q);
        }
        /* Move the remaining incomplete line to the start */
        nfilled = end-q;
        memmove(buffer, q, nfilled);
        if (sizehint > 0)
            if (totalread >= (size_t)sizehint)
                break;
        if (shortread) {
            sizehint = 0;
            break;
        }
    }
    if (nfilled != 0) {
        /* Partial last line */
        line = PyString_FromStringAndSize(buffer, nfilled);
        if (line == NULL)
            goto error;
        if (sizehint > 0) {
            /* Need to complete the last line */
            PyObject *rest = Util_GetLine(self, 0);
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
    RELEASE_LOCK(self);
    if (big_buffer) {
        Py_DECREF(big_buffer);
    }
    return list;
}

PyDoc_STRVAR(BZ2File_xreadlines__doc__,
"xreadlines() -> self\n\
\n\
For backward compatibility. BZ2File objects now include the performance\n\
optimizations previously implemented in the xreadlines module.\n\
");

PyDoc_STRVAR(BZ2File_write__doc__,
"write(data) -> None\n\
\n\
Write the 'data' string to file. Note that due to buffering, close() may\n\
be needed before the file on disk reflects the data written.\n\
");

/* This is a hacked version of Python's fileobject.c:file_write(). */
static PyObject *
BZ2File_write(BZ2FileObject *self, PyObject *args)
{
    PyObject *ret = NULL;
    Py_buffer pbuf;
    char *buf;
    int len;
    int bzerror;

    if (!PyArg_ParseTuple(args, "s*:write", &pbuf))
        return NULL;
    buf = pbuf.buf;
    len = pbuf.len;

    ACQUIRE_LOCK(self);
    switch (self->mode) {
        case MODE_WRITE:
            break;

        case MODE_CLOSED:
            PyErr_SetString(PyExc_ValueError,
                            "I/O operation on closed file");
            goto cleanup;

        default:
            PyErr_SetString(PyExc_IOError,
                            "file is not ready for writing");
            goto cleanup;
    }

    self->f_softspace = 0;

    Py_BEGIN_ALLOW_THREADS
    BZ2_bzWrite (&bzerror, self->fp, buf, len);
    self->pos += len;
    Py_END_ALLOW_THREADS

    if (bzerror != BZ_OK) {
        Util_CatchBZ2Error(bzerror);
        goto cleanup;
    }

    Py_INCREF(Py_None);
    ret = Py_None;

cleanup:
    PyBuffer_Release(&pbuf);
    RELEASE_LOCK(self);
    return ret;
}

PyDoc_STRVAR(BZ2File_writelines__doc__,
"writelines(sequence_of_strings) -> None\n\
\n\
Write the sequence of strings to the file. Note that newlines are not\n\
added. The sequence can be any iterable object producing strings. This is\n\
equivalent to calling write() for each string.\n\
");

/* This is a hacked version of Python's fileobject.c:file_writelines(). */
static PyObject *
BZ2File_writelines(BZ2FileObject *self, PyObject *seq)
{
#define CHUNKSIZE 1000
    PyObject *list = NULL;
    PyObject *iter = NULL;
    PyObject *ret = NULL;
    PyObject *line;
    int i, j, index, len, islist;
    int bzerror;

    ACQUIRE_LOCK(self);
    switch (self->mode) {
        case MODE_WRITE:
            break;

        case MODE_CLOSED:
            PyErr_SetString(PyExc_ValueError,
                            "I/O operation on closed file");
            goto error;

        default:
            PyErr_SetString(PyExc_IOError,
                            "file is not ready for writing");
            goto error;
    }

    islist = PyList_Check(seq);
    if  (!islist) {
        iter = PyObject_GetIter(seq);
        if (iter == NULL) {
            PyErr_SetString(PyExc_TypeError,
                "writelines() requires an iterable argument");
            goto error;
        }
        list = PyList_New(CHUNKSIZE);
        if (list == NULL)
            goto error;
    }

    /* Strategy: slurp CHUNKSIZE lines into a private list,
       checking that they are all strings, then write that list
       without holding the interpreter lock, then come back for more. */
    for (index = 0; ; index += CHUNKSIZE) {
        if (islist) {
            Py_XDECREF(list);
            list = PyList_GetSlice(seq, index, index+CHUNKSIZE);
            if (list == NULL)
                goto error;
            j = PyList_GET_SIZE(list);
        }
        else {
            for (j = 0; j < CHUNKSIZE; j++) {
                line = PyIter_Next(iter);
                if (line == NULL) {
                    if (PyErr_Occurred())
                        goto error;
                    break;
                }
                PyList_SetItem(list, j, line);
            }
        }
        if (j == 0)
            break;

        /* Check that all entries are indeed strings. If not,
           apply the same rules as for file.write() and
           convert the rets to strings. This is slow, but
           seems to be the only way since all conversion APIs
           could potentially execute Python code. */
        for (i = 0; i < j; i++) {
            PyObject *v = PyList_GET_ITEM(list, i);
            if (!PyString_Check(v)) {
                const char *buffer;
                Py_ssize_t len;
                if (PyObject_AsCharBuffer(v, &buffer, &len)) {
                    PyErr_SetString(PyExc_TypeError,
                                    "writelines() "
                                    "argument must be "
                                    "a sequence of "
                                    "strings");
                    goto error;
                }
                line = PyString_FromStringAndSize(buffer,
                                                  len);
                if (line == NULL)
                    goto error;
                Py_DECREF(v);
                PyList_SET_ITEM(list, i, line);
            }
        }

        self->f_softspace = 0;

        /* Since we are releasing the global lock, the
           following code may *not* execute Python code. */
        Py_BEGIN_ALLOW_THREADS
        for (i = 0; i < j; i++) {
            line = PyList_GET_ITEM(list, i);
            len = PyString_GET_SIZE(line);
            BZ2_bzWrite (&bzerror, self->fp,
                         PyString_AS_STRING(line), len);
            if (bzerror != BZ_OK) {
                Py_BLOCK_THREADS
                Util_CatchBZ2Error(bzerror);
                goto error;
            }
        }
        Py_END_ALLOW_THREADS

        if (j < CHUNKSIZE)
            break;
    }

    Py_INCREF(Py_None);
    ret = Py_None;

  error:
    RELEASE_LOCK(self);
    Py_XDECREF(list);
    Py_XDECREF(iter);
    return ret;
#undef CHUNKSIZE
}

PyDoc_STRVAR(BZ2File_seek__doc__,
"seek(offset [, whence]) -> None\n\
\n\
Move to new file position. Argument offset is a byte count. Optional\n\
argument whence defaults to 0 (offset from start of file, offset\n\
should be >= 0); other values are 1 (move relative to current position,\n\
positive or negative), and 2 (move relative to end of file, usually\n\
negative, although many platforms allow seeking beyond the end of a file).\n\
\n\
Note that seeking of bz2 files is emulated, and depending on the parameters\n\
the operation may be extremely slow.\n\
");

static PyObject *
BZ2File_seek(BZ2FileObject *self, PyObject *args)
{
    int where = 0;
    PyObject *offobj;
    Py_off_t offset;
    char small_buffer[SMALLCHUNK];
    char *buffer = small_buffer;
    size_t buffersize = SMALLCHUNK;
    Py_off_t bytesread = 0;
    size_t readsize;
    int chunksize;
    int bzerror;
    PyObject *ret = NULL;

    if (!PyArg_ParseTuple(args, "O|i:seek", &offobj, &where))
        return NULL;
#if !defined(HAVE_LARGEFILE_SUPPORT)
    offset = PyInt_AsLong(offobj);
#else
    offset = PyLong_Check(offobj) ?
        PyLong_AsLongLong(offobj) : PyInt_AsLong(offobj);
#endif
    if (PyErr_Occurred())
        return NULL;

    ACQUIRE_LOCK(self);
    Util_DropReadAhead(self);
    switch (self->mode) {
        case MODE_READ:
        case MODE_READ_EOF:
            break;

        case MODE_CLOSED:
            PyErr_SetString(PyExc_ValueError,
                            "I/O operation on closed file");
            goto cleanup;

        default:
            PyErr_SetString(PyExc_IOError,
                            "seek works only while reading");
            goto cleanup;
    }

    if (where == 2) {
        if (self->size == -1) {
            assert(self->mode != MODE_READ_EOF);
            for (;;) {
                Py_BEGIN_ALLOW_THREADS
                chunksize = Util_UnivNewlineRead(
                                &bzerror, self->fp,
                                buffer, buffersize,
                                self);
                self->pos += chunksize;
                Py_END_ALLOW_THREADS

                bytesread += chunksize;
                if (bzerror == BZ_STREAM_END) {
                    break;
                } else if (bzerror != BZ_OK) {
                    Util_CatchBZ2Error(bzerror);
                    goto cleanup;
                }
            }
            self->mode = MODE_READ_EOF;
            self->size = self->pos;
            bytesread = 0;
        }
        offset = self->size + offset;
    } else if (where == 1) {
        offset = self->pos + offset;
    }

    /* Before getting here, offset must be the absolute position the file
     * pointer should be set to. */

    if (offset >= self->pos) {
        /* we can move forward */
        offset -= self->pos;
    } else {
        /* we cannot move back, so rewind the stream */
        BZ2_bzReadClose(&bzerror, self->fp);
        if (self->fp) {
            PyFile_DecUseCount((PyFileObject *)self->file);
            self->fp = NULL;
        }
        if (bzerror != BZ_OK) {
            Util_CatchBZ2Error(bzerror);
            goto cleanup;
        }
        ret = PyObject_CallMethod(self->file, "seek", "(i)", 0);
        if (!ret)
            goto cleanup;
        Py_DECREF(ret);
        ret = NULL;
        self->pos = 0;
        self->fp = BZ2_bzReadOpen(&bzerror, PyFile_AsFile(self->file),
                                  0, 0, NULL, 0);
        if (self->fp)
            PyFile_IncUseCount((PyFileObject *)self->file);
        if (bzerror != BZ_OK) {
            Util_CatchBZ2Error(bzerror);
            goto cleanup;
        }
        self->mode = MODE_READ;
    }

    if (offset <= 0 || self->mode == MODE_READ_EOF)
        goto exit;

    /* Before getting here, offset must be set to the number of bytes
     * to walk forward. */
    for (;;) {
        if (offset-bytesread > buffersize)
            readsize = buffersize;
        else
            /* offset might be wider that readsize, but the result
             * of the subtraction is bound by buffersize (see the
             * condition above). buffersize is 8192. */
            readsize = (size_t)(offset-bytesread);
        Py_BEGIN_ALLOW_THREADS
        chunksize = Util_UnivNewlineRead(&bzerror, self->fp,
                                         buffer, readsize, self);
        self->pos += chunksize;
        Py_END_ALLOW_THREADS
        bytesread += chunksize;
        if (bzerror == BZ_STREAM_END) {
            self->size = self->pos;
            self->mode = MODE_READ_EOF;
            break;
        } else if (bzerror != BZ_OK) {
            Util_CatchBZ2Error(bzerror);
            goto cleanup;
        }
        if (bytesread == offset)
            break;
    }

exit:
    Py_INCREF(Py_None);
    ret = Py_None;

cleanup:
    RELEASE_LOCK(self);
    return ret;
}

PyDoc_STRVAR(BZ2File_tell__doc__,
"tell() -> int\n\
\n\
Return the current file position, an integer (may be a long integer).\n\
");

static PyObject *
BZ2File_tell(BZ2FileObject *self, PyObject *args)
{
    PyObject *ret = NULL;

    if (self->mode == MODE_CLOSED) {
        PyErr_SetString(PyExc_ValueError,
                        "I/O operation on closed file");
        goto cleanup;
    }

#if !defined(HAVE_LARGEFILE_SUPPORT)
    ret = PyInt_FromLong(self->pos);
#else
    ret = PyLong_FromLongLong(self->pos);
#endif

cleanup:
    return ret;
}

PyDoc_STRVAR(BZ2File_close__doc__,
"close() -> None or (perhaps) an integer\n\
\n\
Close the file. Sets data attribute .closed to true. A closed file\n\
cannot be used for further I/O operations. close() may be called more\n\
than once without error.\n\
");

static PyObject *
BZ2File_close(BZ2FileObject *self)
{
    PyObject *ret = NULL;
    int bzerror = BZ_OK;

    ACQUIRE_LOCK(self);
    switch (self->mode) {
        case MODE_READ:
        case MODE_READ_EOF:
            BZ2_bzReadClose(&bzerror, self->fp);
            break;
        case MODE_WRITE:
            BZ2_bzWriteClose(&bzerror, self->fp,
                             0, NULL, NULL);
            break;
    }
    if (self->file) {
        if (self->fp)
            PyFile_DecUseCount((PyFileObject *)self->file);
        ret = PyObject_CallMethod(self->file, "close", NULL);
    } else {
        Py_INCREF(Py_None);
        ret = Py_None;
    }
    self->fp = NULL;
    self->mode = MODE_CLOSED;
    if (bzerror != BZ_OK) {
        Util_CatchBZ2Error(bzerror);
        Py_XDECREF(ret);
        ret = NULL;
    }

    RELEASE_LOCK(self);
    return ret;
}

PyDoc_STRVAR(BZ2File_enter_doc,
"__enter__() -> self.");

static PyObject *
BZ2File_enter(BZ2FileObject *self)
{
    if (self->mode == MODE_CLOSED) {
        PyErr_SetString(PyExc_ValueError,
            "I/O operation on closed file");
        return NULL;
    }
    Py_INCREF(self);
    return (PyObject *) self;
}

PyDoc_STRVAR(BZ2File_exit_doc,
"__exit__(*excinfo) -> None.  Closes the file.");

static PyObject *
BZ2File_exit(BZ2FileObject *self, PyObject *args)
{
    PyObject *ret = PyObject_CallMethod((PyObject *) self, "close", NULL);
    if (!ret)
        /* If error occurred, pass through */
        return NULL;
    Py_DECREF(ret);
    Py_RETURN_NONE;
}


static PyObject *BZ2File_getiter(BZ2FileObject *self);

static PyMethodDef BZ2File_methods[] = {
    {"read", (PyCFunction)BZ2File_read, METH_VARARGS, BZ2File_read__doc__},
    {"readline", (PyCFunction)BZ2File_readline, METH_VARARGS, BZ2File_readline__doc__},
    {"readlines", (PyCFunction)BZ2File_readlines, METH_VARARGS, BZ2File_readlines__doc__},
    {"xreadlines", (PyCFunction)BZ2File_getiter, METH_VARARGS, BZ2File_xreadlines__doc__},
    {"write", (PyCFunction)BZ2File_write, METH_VARARGS, BZ2File_write__doc__},
    {"writelines", (PyCFunction)BZ2File_writelines, METH_O, BZ2File_writelines__doc__},
    {"seek", (PyCFunction)BZ2File_seek, METH_VARARGS, BZ2File_seek__doc__},
    {"tell", (PyCFunction)BZ2File_tell, METH_NOARGS, BZ2File_tell__doc__},
    {"close", (PyCFunction)BZ2File_close, METH_NOARGS, BZ2File_close__doc__},
    {"__enter__", (PyCFunction)BZ2File_enter, METH_NOARGS, BZ2File_enter_doc},
    {"__exit__", (PyCFunction)BZ2File_exit, METH_VARARGS, BZ2File_exit_doc},
    {NULL,              NULL}           /* sentinel */
};


/* ===================================================================== */
/* Getters and setters of BZ2File. */

/* This is a hacked version of Python's fileobject.c:get_newlines(). */
static PyObject *
BZ2File_get_newlines(BZ2FileObject *self, void *closure)
{
    switch (self->f_newlinetypes) {
    case NEWLINE_UNKNOWN:
        Py_INCREF(Py_None);
        return Py_None;
    case NEWLINE_CR:
        return PyString_FromString("\r");
    case NEWLINE_LF:
        return PyString_FromString("\n");
    case NEWLINE_CR|NEWLINE_LF:
        return Py_BuildValue("(ss)", "\r", "\n");
    case NEWLINE_CRLF:
        return PyString_FromString("\r\n");
    case NEWLINE_CR|NEWLINE_CRLF:
        return Py_BuildValue("(ss)", "\r", "\r\n");
    case NEWLINE_LF|NEWLINE_CRLF:
        return Py_BuildValue("(ss)", "\n", "\r\n");
    case NEWLINE_CR|NEWLINE_LF|NEWLINE_CRLF:
        return Py_BuildValue("(sss)", "\r", "\n", "\r\n");
    default:
        PyErr_Format(PyExc_SystemError,
                     "Unknown newlines value 0x%x\n",
                     self->f_newlinetypes);
        return NULL;
    }
}

static PyObject *
BZ2File_get_closed(BZ2FileObject *self, void *closure)
{
    return PyInt_FromLong(self->mode == MODE_CLOSED);
}

static PyObject *
BZ2File_get_mode(BZ2FileObject *self, void *closure)
{
    return PyObject_GetAttrString(self->file, "mode");
}

static PyObject *
BZ2File_get_name(BZ2FileObject *self, void *closure)
{
    return PyObject_GetAttrString(self->file, "name");
}

static PyGetSetDef BZ2File_getset[] = {
    {"closed", (getter)BZ2File_get_closed, NULL,
                    "True if the file is closed"},
    {"newlines", (getter)BZ2File_get_newlines, NULL,
                    "end-of-line convention used in this file"},
    {"mode", (getter)BZ2File_get_mode, NULL,
                    "file mode ('r', 'w', or 'U')"},
    {"name", (getter)BZ2File_get_name, NULL,
                    "file name"},
    {NULL}      /* Sentinel */
};


/* ===================================================================== */
/* Members of BZ2File_Type. */

#undef OFF
#define OFF(x) offsetof(BZ2FileObject, x)

static PyMemberDef BZ2File_members[] = {
    {"softspace",       T_INT,          OFF(f_softspace), 0,
     "flag indicating that a space needs to be printed; used by print"},
    {NULL}      /* Sentinel */
};

/* ===================================================================== */
/* Slot definitions for BZ2File_Type. */

static int
BZ2File_init(BZ2FileObject *self, PyObject *args, PyObject *kwargs)
{
    static char *kwlist[] = {"filename", "mode", "buffering",
                                   "compresslevel", 0};
    PyObject *name;
    char *mode = "r";
    int buffering = -1;
    int compresslevel = 9;
    int bzerror;
    int mode_char = 0;

    self->size = -1;

    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "O|sii:BZ2File",
                                     kwlist, &name, &mode, &buffering,
                                     &compresslevel))
        return -1;

    if (compresslevel < 1 || compresslevel > 9) {
        PyErr_SetString(PyExc_ValueError,
                        "compresslevel must be between 1 and 9");
        return -1;
    }

    for (;;) {
        int error = 0;
        switch (*mode) {
            case 'r':
            case 'w':
                if (mode_char)
                    error = 1;
                mode_char = *mode;
                break;

            case 'b':
                break;

            case 'U':
#ifdef __VMS
                self->f_univ_newline = 0;
#else
                self->f_univ_newline = 1;
#endif
                break;

            default:
                error = 1;
                break;
        }
        if (error) {
            PyErr_Format(PyExc_ValueError,
                         "invalid mode char %c", *mode);
            return -1;
        }
        mode++;
        if (*mode == '\0')
            break;
    }

    if (mode_char == 0) {
        mode_char = 'r';
    }

    mode = (mode_char == 'r') ? "rb" : "wb";

    self->file = PyObject_CallFunction((PyObject*)&PyFile_Type, "(Osi)",
                                       name, mode, buffering);
    if (self->file == NULL)
        return -1;

    /* From now on, we have stuff to dealloc, so jump to error label
     * instead of returning */

#ifdef WITH_THREAD
    self->lock = PyThread_allocate_lock();
    if (!self->lock) {
        PyErr_SetString(PyExc_MemoryError, "unable to allocate lock");
        goto error;
    }
#endif

    if (mode_char == 'r')
        self->fp = BZ2_bzReadOpen(&bzerror,
                                  PyFile_AsFile(self->file),
                                  0, 0, NULL, 0);
    else
        self->fp = BZ2_bzWriteOpen(&bzerror,
                                   PyFile_AsFile(self->file),
                                   compresslevel, 0, 0);

    if (bzerror != BZ_OK) {
        Util_CatchBZ2Error(bzerror);
        goto error;
    }
    PyFile_IncUseCount((PyFileObject *)self->file);

    self->mode = (mode_char == 'r') ? MODE_READ : MODE_WRITE;

    return 0;

error:
    Py_CLEAR(self->file);
#ifdef WITH_THREAD
    if (self->lock) {
        PyThread_free_lock(self->lock);
        self->lock = NULL;
    }
#endif
    return -1;
}

static void
BZ2File_dealloc(BZ2FileObject *self)
{
    int bzerror;
#ifdef WITH_THREAD
    if (self->lock)
        PyThread_free_lock(self->lock);
#endif
    switch (self->mode) {
        case MODE_READ:
        case MODE_READ_EOF:
            BZ2_bzReadClose(&bzerror, self->fp);
            break;
        case MODE_WRITE:
            BZ2_bzWriteClose(&bzerror, self->fp,
                             0, NULL, NULL);
            break;
    }
    if (self->fp != NULL && self->file != NULL)
        PyFile_DecUseCount((PyFileObject *)self->file);
    self->fp = NULL;
    Util_DropReadAhead(self);
    Py_XDECREF(self->file);
    Py_TYPE(self)->tp_free((PyObject *)self);
}

/* This is a hacked version of Python's fileobject.c:file_getiter(). */
static PyObject *
BZ2File_getiter(BZ2FileObject *self)
{
    if (self->mode == MODE_CLOSED) {
        PyErr_SetString(PyExc_ValueError,
                        "I/O operation on closed file");
        return NULL;
    }
    Py_INCREF((PyObject*)self);
    return (PyObject *)self;
}

/* This is a hacked version of Python's fileobject.c:file_iternext(). */
#define READAHEAD_BUFSIZE 8192
static PyObject *
BZ2File_iternext(BZ2FileObject *self)
{
    PyStringObject* ret;
    ACQUIRE_LOCK(self);
    if (self->mode == MODE_CLOSED) {
        RELEASE_LOCK(self);
        PyErr_SetString(PyExc_ValueError,
                        "I/O operation on closed file");
        return NULL;
    }
    ret = Util_ReadAheadGetLineSkip(self, 0, READAHEAD_BUFSIZE);
    RELEASE_LOCK(self);
    if (ret == NULL || PyString_GET_SIZE(ret) == 0) {
        Py_XDECREF(ret);
        return NULL;
    }
    return (PyObject *)ret;
}

/* ===================================================================== */
/* BZ2File_Type definition. */

PyDoc_VAR(BZ2File__doc__) =
PyDoc_STR(
"BZ2File(name [, mode='r', buffering=0, compresslevel=9]) -> file object\n\
\n\
Open a bz2 file. The mode can be 'r' or 'w', for reading (default) or\n\
writing. When opened for writing, the file will be created if it doesn't\n\
exist, and truncated otherwise. If the buffering argument is given, 0 means\n\
unbuffered, and larger numbers specify the buffer size. If compresslevel\n\
is given, must be a number between 1 and 9.\n\
")
PyDoc_STR(
"\n\
Add a 'U' to mode to open the file for input with universal newline\n\
support. Any line ending in the input file will be seen as a '\\n' in\n\
Python. Also, a file so opened gains the attribute 'newlines'; the value\n\
for this attribute is one of None (no newline read yet), '\\r', '\\n',\n\
'\\r\\n' or a tuple containing all the newline types seen. Universal\n\
newlines are available only when reading.\n\
")
;

static PyTypeObject BZ2File_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "bz2.BZ2File",              /*tp_name*/
    sizeof(BZ2FileObject),      /*tp_basicsize*/
    0,                          /*tp_itemsize*/
    (destructor)BZ2File_dealloc, /*tp_dealloc*/
    0,                          /*tp_print*/
    0,                          /*tp_getattr*/
    0,                          /*tp_setattr*/
    0,                          /*tp_compare*/
    0,                          /*tp_repr*/
    0,                          /*tp_as_number*/
    0,                          /*tp_as_sequence*/
    0,                          /*tp_as_mapping*/
    0,                          /*tp_hash*/
    0,                      /*tp_call*/
    0,                      /*tp_str*/
    PyObject_GenericGetAttr,/*tp_getattro*/
    PyObject_GenericSetAttr,/*tp_setattro*/
    0,                      /*tp_as_buffer*/
    Py_TPFLAGS_DEFAULT|Py_TPFLAGS_BASETYPE, /*tp_flags*/
    BZ2File__doc__,         /*tp_doc*/
    0,                      /*tp_traverse*/
    0,                      /*tp_clear*/
    0,                      /*tp_richcompare*/
    0,                      /*tp_weaklistoffset*/
    (getiterfunc)BZ2File_getiter, /*tp_iter*/
    (iternextfunc)BZ2File_iternext, /*tp_iternext*/
    BZ2File_methods,        /*tp_methods*/
    BZ2File_members,        /*tp_members*/
    BZ2File_getset,         /*tp_getset*/
    0,                      /*tp_base*/
    0,                      /*tp_dict*/
    0,                      /*tp_descr_get*/
    0,                      /*tp_descr_set*/
    0,                      /*tp_dictoffset*/
    (initproc)BZ2File_init, /*tp_init*/
    PyType_GenericAlloc,    /*tp_alloc*/
    PyType_GenericNew,      /*tp_new*/
    _PyObject_Del,          /*tp_free*/
    0,                      /*tp_is_gc*/
};


/* ===================================================================== */
/* Methods of BZ2Comp. */

PyDoc_STRVAR(BZ2Comp_compress__doc__,
"compress(data) -> string\n\
\n\
Provide more data to the compressor object. It will return chunks of\n\
compressed data whenever possible. When you've finished providing data\n\
to compress, call the flush() method to finish the compression process,\n\
and return what is left in the internal buffers.\n\
");

static PyObject *
BZ2Comp_compress(BZ2CompObject *self, PyObject *args)
{
    Py_buffer pdata;
    size_t input_left;
    size_t output_size = 0;
    PyObject *ret = NULL;
    bz_stream *bzs = &self->bzs;
    int bzerror;

    if (!PyArg_ParseTuple(args, "s*:compress", &pdata))
        return NULL;

    if (pdata.len == 0) {
        PyBuffer_Release(&pdata);
        return PyString_FromString("");
    }

    ACQUIRE_LOCK(self);
    if (!self->running) {
        PyErr_SetString(PyExc_ValueError,
                        "this object was already flushed");
        goto error;
    }

    ret = PyString_FromStringAndSize(NULL, SMALLCHUNK);
    if (!ret)
        goto error;

    bzs->next_in = pdata.buf;
    bzs->avail_in = MIN(pdata.len, UINT_MAX);
    input_left = pdata.len - bzs->avail_in;

    bzs->next_out = BUF(ret);
    bzs->avail_out = PyString_GET_SIZE(ret);

    for (;;) {
        char *saved_next_out;

        Py_BEGIN_ALLOW_THREADS
        saved_next_out = bzs->next_out;
        bzerror = BZ2_bzCompress(bzs, BZ_RUN);
        output_size += bzs->next_out - saved_next_out;
        Py_END_ALLOW_THREADS

        if (bzerror != BZ_RUN_OK) {
            Util_CatchBZ2Error(bzerror);
            goto error;
        }
        if (bzs->avail_in == 0) {
            if (input_left == 0)
                break; /* no more input data */
            bzs->avail_in = MIN(input_left, UINT_MAX);
            input_left -= bzs->avail_in;
        }
        if (bzs->avail_out == 0) {
            size_t buffer_left = PyString_GET_SIZE(ret) - output_size;
            if (buffer_left == 0) {
                if (Util_GrowBuffer(&ret) < 0) {
                    BZ2_bzCompressEnd(bzs);
                    goto error;
                }
                bzs->next_out = BUF(ret) + output_size;
                buffer_left = PyString_GET_SIZE(ret) - output_size;
            }
            bzs->avail_out = MIN(buffer_left, UINT_MAX);
        }
    }

    if (_PyString_Resize(&ret, output_size) < 0)
        goto error;

    RELEASE_LOCK(self);
    PyBuffer_Release(&pdata);
    return ret;

error:
    RELEASE_LOCK(self);
    PyBuffer_Release(&pdata);
    Py_XDECREF(ret);
    return NULL;
}

PyDoc_STRVAR(BZ2Comp_flush__doc__,
"flush() -> string\n\
\n\
Finish the compression process and return what is left in internal buffers.\n\
You must not use the compressor object after calling this method.\n\
");

static PyObject *
BZ2Comp_flush(BZ2CompObject *self)
{
    size_t output_size = 0;
    PyObject *ret = NULL;
    bz_stream *bzs = &self->bzs;
    int bzerror;

    ACQUIRE_LOCK(self);
    if (!self->running) {
        PyErr_SetString(PyExc_ValueError, "object was already flushed");
        goto error;
    }
    self->running = 0;

    ret = PyString_FromStringAndSize(NULL, SMALLCHUNK);
    if (!ret)
        goto error;

    bzs->next_out = BUF(ret);
    bzs->avail_out = PyString_GET_SIZE(ret);

    for (;;) {
        char *saved_next_out;

        Py_BEGIN_ALLOW_THREADS
        saved_next_out = bzs->next_out;
        bzerror = BZ2_bzCompress(bzs, BZ_FINISH);
        output_size += bzs->next_out - saved_next_out;
        Py_END_ALLOW_THREADS

        if (bzerror == BZ_STREAM_END) {
            break;
        } else if (bzerror != BZ_FINISH_OK) {
            Util_CatchBZ2Error(bzerror);
            goto error;
        }
        if (bzs->avail_out == 0) {
            size_t buffer_left = PyString_GET_SIZE(ret) - output_size;
            if (buffer_left == 0) {
                if (Util_GrowBuffer(&ret) < 0)
                    goto error;
                bzs->next_out = BUF(ret) + output_size;
                buffer_left = PyString_GET_SIZE(ret) - output_size;
            }
            bzs->avail_out = MIN(buffer_left, UINT_MAX);
        }
    }

    if (output_size != PyString_GET_SIZE(ret))
        if (_PyString_Resize(&ret, output_size) < 0)
            goto error;

    RELEASE_LOCK(self);
    return ret;

error:
    RELEASE_LOCK(self);
    Py_XDECREF(ret);
    return NULL;
}

static PyMethodDef BZ2Comp_methods[] = {
    {"compress", (PyCFunction)BZ2Comp_compress, METH_VARARGS,
     BZ2Comp_compress__doc__},
    {"flush", (PyCFunction)BZ2Comp_flush, METH_NOARGS,
     BZ2Comp_flush__doc__},
    {NULL,              NULL}           /* sentinel */
};


/* ===================================================================== */
/* Slot definitions for BZ2Comp_Type. */

static int
BZ2Comp_init(BZ2CompObject *self, PyObject *args, PyObject *kwargs)
{
    int compresslevel = 9;
    int bzerror;
    static char *kwlist[] = {"compresslevel", 0};

    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "|i:BZ2Compressor",
                                     kwlist, &compresslevel))
        return -1;

    if (compresslevel < 1 || compresslevel > 9) {
        PyErr_SetString(PyExc_ValueError,
                        "compresslevel must be between 1 and 9");
        goto error;
    }

#ifdef WITH_THREAD
    self->lock = PyThread_allocate_lock();
    if (!self->lock) {
        PyErr_SetString(PyExc_MemoryError, "unable to allocate lock");
        goto error;
    }
#endif

    memset(&self->bzs, 0, sizeof(bz_stream));
    bzerror = BZ2_bzCompressInit(&self->bzs, compresslevel, 0, 0);
    if (bzerror != BZ_OK) {
        Util_CatchBZ2Error(bzerror);
        goto error;
    }

    self->running = 1;

    return 0;
error:
#ifdef WITH_THREAD
    if (self->lock) {
        PyThread_free_lock(self->lock);
        self->lock = NULL;
    }
#endif
    return -1;
}

static void
BZ2Comp_dealloc(BZ2CompObject *self)
{
#ifdef WITH_THREAD
    if (self->lock)
        PyThread_free_lock(self->lock);
#endif
    BZ2_bzCompressEnd(&self->bzs);
    Py_TYPE(self)->tp_free((PyObject *)self);
}


/* ===================================================================== */
/* BZ2Comp_Type definition. */

PyDoc_STRVAR(BZ2Comp__doc__,
"BZ2Compressor([compresslevel=9]) -> compressor object\n\
\n\
Create a new compressor object. This object may be used to compress\n\
data sequentially. If you want to compress data in one shot, use the\n\
compress() function instead. The compresslevel parameter, if given,\n\
must be a number between 1 and 9.\n\
");

static PyTypeObject BZ2Comp_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "bz2.BZ2Compressor",        /*tp_name*/
    sizeof(BZ2CompObject),      /*tp_basicsize*/
    0,                          /*tp_itemsize*/
    (destructor)BZ2Comp_dealloc, /*tp_dealloc*/
    0,                          /*tp_print*/
    0,                          /*tp_getattr*/
    0,                          /*tp_setattr*/
    0,                          /*tp_compare*/
    0,                          /*tp_repr*/
    0,                          /*tp_as_number*/
    0,                          /*tp_as_sequence*/
    0,                          /*tp_as_mapping*/
    0,                          /*tp_hash*/
    0,                      /*tp_call*/
    0,                      /*tp_str*/
    PyObject_GenericGetAttr,/*tp_getattro*/
    PyObject_GenericSetAttr,/*tp_setattro*/
    0,                      /*tp_as_buffer*/
    Py_TPFLAGS_DEFAULT|Py_TPFLAGS_BASETYPE, /*tp_flags*/
    BZ2Comp__doc__,         /*tp_doc*/
    0,                      /*tp_traverse*/
    0,                      /*tp_clear*/
    0,                      /*tp_richcompare*/
    0,                      /*tp_weaklistoffset*/
    0,                      /*tp_iter*/
    0,                      /*tp_iternext*/
    BZ2Comp_methods,        /*tp_methods*/
    0,                      /*tp_members*/
    0,                      /*tp_getset*/
    0,                      /*tp_base*/
    0,                      /*tp_dict*/
    0,                      /*tp_descr_get*/
    0,                      /*tp_descr_set*/
    0,                      /*tp_dictoffset*/
    (initproc)BZ2Comp_init, /*tp_init*/
    PyType_GenericAlloc,    /*tp_alloc*/
    PyType_GenericNew,      /*tp_new*/
    _PyObject_Del,          /*tp_free*/
    0,                      /*tp_is_gc*/
};


/* ===================================================================== */
/* Members of BZ2Decomp. */

#undef OFF
#define OFF(x) offsetof(BZ2DecompObject, x)

static PyMemberDef BZ2Decomp_members[] = {
    {"unused_data", T_OBJECT, OFF(unused_data), RO},
    {NULL}      /* Sentinel */
};


/* ===================================================================== */
/* Methods of BZ2Decomp. */

PyDoc_STRVAR(BZ2Decomp_decompress__doc__,
"decompress(data) -> string\n\
\n\
Provide more data to the decompressor object. It will return chunks\n\
of decompressed data whenever possible. If you try to decompress data\n\
after the end of stream is found, EOFError will be raised. If any data\n\
was found after the end of stream, it'll be ignored and saved in\n\
unused_data attribute.\n\
");

static PyObject *
BZ2Decomp_decompress(BZ2DecompObject *self, PyObject *args)
{
    Py_buffer pdata;
    size_t input_left;
    size_t output_size = 0;
    PyObject *ret = NULL;
    bz_stream *bzs = &self->bzs;
    int bzerror;

    if (!PyArg_ParseTuple(args, "s*:decompress", &pdata))
        return NULL;

    ACQUIRE_LOCK(self);
    if (!self->running) {
        PyErr_SetString(PyExc_EOFError, "end of stream was "
                                        "already found");
        goto error;
    }

    ret = PyString_FromStringAndSize(NULL, SMALLCHUNK);
    if (!ret)
        goto error;

    bzs->next_in = pdata.buf;
    bzs->avail_in = MIN(pdata.len, UINT_MAX);
    input_left = pdata.len - bzs->avail_in;

    bzs->next_out = BUF(ret);
    bzs->avail_out = PyString_GET_SIZE(ret);

    for (;;) {
        char *saved_next_out;

        Py_BEGIN_ALLOW_THREADS
        saved_next_out = bzs->next_out;
        bzerror = BZ2_bzDecompress(bzs);
        output_size += bzs->next_out - saved_next_out;
        Py_END_ALLOW_THREADS

        if (bzerror == BZ_STREAM_END) {
            self->running = 0;
            input_left += bzs->avail_in;
            if (input_left != 0) {
                Py_DECREF(self->unused_data);
                self->unused_data =
                    PyString_FromStringAndSize(bzs->next_in, input_left);
                if (self->unused_data == NULL)
                    goto error;
            }
            break;
        }
        if (bzerror != BZ_OK) {
            Util_CatchBZ2Error(bzerror);
            goto error;
        }
        if (bzs->avail_in == 0) {
            if (input_left == 0)
                break; /* no more input data */
            bzs->avail_in = MIN(input_left, UINT_MAX);
            input_left -= bzs->avail_in;
        }
        if (bzs->avail_out == 0) {
            size_t buffer_left = PyString_GET_SIZE(ret) - output_size;
            if (buffer_left == 0) {
                if (Util_GrowBuffer(&ret) < 0) {
                    BZ2_bzDecompressEnd(bzs);
                    goto error;
                }
                bzs->next_out = BUF(ret) + output_size;
                buffer_left = PyString_GET_SIZE(ret) - output_size;
            }
            bzs->avail_out = MIN(buffer_left, UINT_MAX);
        }
    }

    if (output_size != PyString_GET_SIZE(ret))
        if (_PyString_Resize(&ret, output_size) < 0)
            goto error;

    RELEASE_LOCK(self);
    PyBuffer_Release(&pdata);
    return ret;

error:
    RELEASE_LOCK(self);
    PyBuffer_Release(&pdata);
    Py_XDECREF(ret);
    return NULL;
}

static PyMethodDef BZ2Decomp_methods[] = {
    {"decompress", (PyCFunction)BZ2Decomp_decompress, METH_VARARGS, BZ2Decomp_decompress__doc__},
    {NULL,              NULL}           /* sentinel */
};


/* ===================================================================== */
/* Slot definitions for BZ2Decomp_Type. */

static int
BZ2Decomp_init(BZ2DecompObject *self, PyObject *args, PyObject *kwargs)
{
    int bzerror;

    if (!PyArg_ParseTuple(args, ":BZ2Decompressor"))
        return -1;

#ifdef WITH_THREAD
    self->lock = PyThread_allocate_lock();
    if (!self->lock) {
        PyErr_SetString(PyExc_MemoryError, "unable to allocate lock");
        goto error;
    }
#endif

    self->unused_data = PyString_FromString("");
    if (!self->unused_data)
        goto error;

    memset(&self->bzs, 0, sizeof(bz_stream));
    bzerror = BZ2_bzDecompressInit(&self->bzs, 0, 0);
    if (bzerror != BZ_OK) {
        Util_CatchBZ2Error(bzerror);
        goto error;
    }

    self->running = 1;

    return 0;

error:
#ifdef WITH_THREAD
    if (self->lock) {
        PyThread_free_lock(self->lock);
        self->lock = NULL;
    }
#endif
    Py_CLEAR(self->unused_data);
    return -1;
}

static void
BZ2Decomp_dealloc(BZ2DecompObject *self)
{
#ifdef WITH_THREAD
    if (self->lock)
        PyThread_free_lock(self->lock);
#endif
    Py_XDECREF(self->unused_data);
    BZ2_bzDecompressEnd(&self->bzs);
    Py_TYPE(self)->tp_free((PyObject *)self);
}


/* ===================================================================== */
/* BZ2Decomp_Type definition. */

PyDoc_STRVAR(BZ2Decomp__doc__,
"BZ2Decompressor() -> decompressor object\n\
\n\
Create a new decompressor object. This object may be used to decompress\n\
data sequentially. If you want to decompress data in one shot, use the\n\
decompress() function instead.\n\
");

static PyTypeObject BZ2Decomp_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "bz2.BZ2Decompressor",      /*tp_name*/
    sizeof(BZ2DecompObject), /*tp_basicsize*/
    0,                          /*tp_itemsize*/
    (destructor)BZ2Decomp_dealloc, /*tp_dealloc*/
    0,                          /*tp_print*/
    0,                          /*tp_getattr*/
    0,                          /*tp_setattr*/
    0,                          /*tp_compare*/
    0,                          /*tp_repr*/
    0,                          /*tp_as_number*/
    0,                          /*tp_as_sequence*/
    0,                          /*tp_as_mapping*/
    0,                          /*tp_hash*/
    0,                      /*tp_call*/
    0,                      /*tp_str*/
    PyObject_GenericGetAttr,/*tp_getattro*/
    PyObject_GenericSetAttr,/*tp_setattro*/
    0,                      /*tp_as_buffer*/
    Py_TPFLAGS_DEFAULT|Py_TPFLAGS_BASETYPE, /*tp_flags*/
    BZ2Decomp__doc__,       /*tp_doc*/
    0,                      /*tp_traverse*/
    0,                      /*tp_clear*/
    0,                      /*tp_richcompare*/
    0,                      /*tp_weaklistoffset*/
    0,                      /*tp_iter*/
    0,                      /*tp_iternext*/
    BZ2Decomp_methods,      /*tp_methods*/
    BZ2Decomp_members,      /*tp_members*/
    0,                      /*tp_getset*/
    0,                      /*tp_base*/
    0,                      /*tp_dict*/
    0,                      /*tp_descr_get*/
    0,                      /*tp_descr_set*/
    0,                      /*tp_dictoffset*/
    (initproc)BZ2Decomp_init, /*tp_init*/
    PyType_GenericAlloc,    /*tp_alloc*/
    PyType_GenericNew,      /*tp_new*/
    _PyObject_Del,          /*tp_free*/
    0,                      /*tp_is_gc*/
};


/* ===================================================================== */
/* Module functions. */

PyDoc_STRVAR(bz2_compress__doc__,
"compress(data [, compresslevel=9]) -> string\n\
\n\
Compress data in one shot. If you want to compress data sequentially,\n\
use an instance of BZ2Compressor instead. The compresslevel parameter, if\n\
given, must be a number between 1 and 9.\n\
");

static PyObject *
bz2_compress(PyObject *self, PyObject *args, PyObject *kwargs)
{
    int compresslevel=9;
    int action;
    Py_buffer pdata;
    size_t input_left;
    size_t output_size = 0;
    PyObject *ret = NULL;
    bz_stream _bzs;
    bz_stream *bzs = &_bzs;
    int bzerror;
    static char *kwlist[] = {"data", "compresslevel", 0};

    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "s*|i",
                                     kwlist, &pdata,
                                     &compresslevel))
        return NULL;

    if (compresslevel < 1 || compresslevel > 9) {
        PyErr_SetString(PyExc_ValueError,
                        "compresslevel must be between 1 and 9");
        PyBuffer_Release(&pdata);
        return NULL;
    }

    ret = PyString_FromStringAndSize(NULL, SMALLCHUNK);
    if (!ret) {
        PyBuffer_Release(&pdata);
        return NULL;
    }

    memset(bzs, 0, sizeof(bz_stream));

    bzs->next_in = pdata.buf;
    bzs->avail_in = MIN(pdata.len, UINT_MAX);
    input_left = pdata.len - bzs->avail_in;

    bzs->next_out = BUF(ret);
    bzs->avail_out = PyString_GET_SIZE(ret);

    bzerror = BZ2_bzCompressInit(bzs, compresslevel, 0, 0);
    if (bzerror != BZ_OK) {
        Util_CatchBZ2Error(bzerror);
        PyBuffer_Release(&pdata);
        Py_DECREF(ret);
        return NULL;
    }

    action = input_left > 0 ? BZ_RUN : BZ_FINISH;

    for (;;) {
        char *saved_next_out;

        Py_BEGIN_ALLOW_THREADS
        saved_next_out = bzs->next_out;
        bzerror = BZ2_bzCompress(bzs, action);
        output_size += bzs->next_out - saved_next_out;
        Py_END_ALLOW_THREADS

        if (bzerror == BZ_STREAM_END) {
            break;
        } else if (bzerror != BZ_RUN_OK && bzerror != BZ_FINISH_OK) {
            BZ2_bzCompressEnd(bzs);
            Util_CatchBZ2Error(bzerror);
            PyBuffer_Release(&pdata);
            Py_DECREF(ret);
            return NULL;
        }
        if (action == BZ_RUN && bzs->avail_in == 0) {
            if (input_left == 0) {
                action = BZ_FINISH;
            } else {
                bzs->avail_in = MIN(input_left, UINT_MAX);
                input_left -= bzs->avail_in;
            }
        }
        if (bzs->avail_out == 0) {
            size_t buffer_left = PyString_GET_SIZE(ret) - output_size;
            if (buffer_left == 0) {
                if (Util_GrowBuffer(&ret) < 0) {
                    BZ2_bzCompressEnd(bzs);
                    PyBuffer_Release(&pdata);
                    return NULL;
                }
                bzs->next_out = BUF(ret) + output_size;
                buffer_left = PyString_GET_SIZE(ret) - output_size;
            }
            bzs->avail_out = MIN(buffer_left, UINT_MAX);
        }
    }

    if (output_size != PyString_GET_SIZE(ret))
        _PyString_Resize(&ret, output_size);  /* Sets ret to NULL on failure. */

    BZ2_bzCompressEnd(bzs);
    PyBuffer_Release(&pdata);
    return ret;
}

PyDoc_STRVAR(bz2_decompress__doc__,
"decompress(data) -> decompressed data\n\
\n\
Decompress data in one shot. If you want to decompress data sequentially,\n\
use an instance of BZ2Decompressor instead.\n\
");

static PyObject *
bz2_decompress(PyObject *self, PyObject *args)
{
    Py_buffer pdata;
    size_t input_left;
    size_t output_size = 0;
    PyObject *ret;
    bz_stream _bzs;
    bz_stream *bzs = &_bzs;
    int bzerror;

    if (!PyArg_ParseTuple(args, "s*:decompress", &pdata))
        return NULL;

    if (pdata.len == 0) {
        PyBuffer_Release(&pdata);
        return PyString_FromString("");
    }

    ret = PyString_FromStringAndSize(NULL, SMALLCHUNK);
    if (!ret) {
        PyBuffer_Release(&pdata);
        return NULL;
    }

    memset(bzs, 0, sizeof(bz_stream));

    bzs->next_in = pdata.buf;
    bzs->avail_in = MIN(pdata.len, UINT_MAX);
    input_left = pdata.len - bzs->avail_in;

    bzs->next_out = BUF(ret);
    bzs->avail_out = PyString_GET_SIZE(ret);

    bzerror = BZ2_bzDecompressInit(bzs, 0, 0);
    if (bzerror != BZ_OK) {
        Util_CatchBZ2Error(bzerror);
        Py_DECREF(ret);
        PyBuffer_Release(&pdata);
        return NULL;
    }

    for (;;) {
        char *saved_next_out;

        Py_BEGIN_ALLOW_THREADS
        saved_next_out = bzs->next_out;
        bzerror = BZ2_bzDecompress(bzs);
        output_size += bzs->next_out - saved_next_out;
        Py_END_ALLOW_THREADS

        if (bzerror == BZ_STREAM_END) {
            break;
        } else if (bzerror != BZ_OK) {
            BZ2_bzDecompressEnd(bzs);
            Util_CatchBZ2Error(bzerror);
            PyBuffer_Release(&pdata);
            Py_DECREF(ret);
            return NULL;
        }
        if (bzs->avail_in == 0) {
            if (input_left == 0) {
                BZ2_bzDecompressEnd(bzs);
                PyErr_SetString(PyExc_ValueError,
                                "couldn't find end of stream");
                PyBuffer_Release(&pdata);
                Py_DECREF(ret);
                return NULL;
            }
            bzs->avail_in = MIN(input_left, UINT_MAX);
            input_left -= bzs->avail_in;
        }
        if (bzs->avail_out == 0) {
            size_t buffer_left = PyString_GET_SIZE(ret) - output_size;
            if (buffer_left == 0) {
                if (Util_GrowBuffer(&ret) < 0) {
                    BZ2_bzDecompressEnd(bzs);
                    PyBuffer_Release(&pdata);
                    return NULL;
                }
                bzs->next_out = BUF(ret) + output_size;
                buffer_left = PyString_GET_SIZE(ret) - output_size;
            }
            bzs->avail_out = MIN(buffer_left, UINT_MAX);
        }
    }

    if (output_size != PyString_GET_SIZE(ret))
        _PyString_Resize(&ret, output_size);  /* Sets ret to NULL on failure. */

    BZ2_bzDecompressEnd(bzs);
    PyBuffer_Release(&pdata);
    return ret;
}

static PyMethodDef bz2_methods[] = {
    {"compress", (PyCFunction) bz2_compress, METH_VARARGS|METH_KEYWORDS,
        bz2_compress__doc__},
    {"decompress", (PyCFunction) bz2_decompress, METH_VARARGS,
        bz2_decompress__doc__},
    {NULL,              NULL}           /* sentinel */
};

/* ===================================================================== */
/* Initialization function. */

PyDoc_STRVAR(bz2__doc__,
"The python bz2 module provides a comprehensive interface for\n\
the bz2 compression library. It implements a complete file\n\
interface, one shot (de)compression functions, and types for\n\
sequential (de)compression.\n\
");

PyMODINIT_FUNC
initbz2(void)
{
    PyObject *m;

    if (PyType_Ready(&BZ2File_Type) < 0)
        return;
    if (PyType_Ready(&BZ2Comp_Type) < 0)
        return;
    if (PyType_Ready(&BZ2Decomp_Type) < 0)
        return;

    m = Py_InitModule3("bz2", bz2_methods, bz2__doc__);
    if (m == NULL)
        return;

    PyModule_AddObject(m, "__author__", PyString_FromString(__author__));

    Py_INCREF(&BZ2File_Type);
    PyModule_AddObject(m, "BZ2File", (PyObject *)&BZ2File_Type);

    Py_INCREF(&BZ2Comp_Type);
    PyModule_AddObject(m, "BZ2Compressor", (PyObject *)&BZ2Comp_Type);

    Py_INCREF(&BZ2Decomp_Type);
    PyModule_AddObject(m, "BZ2Decompressor", (PyObject *)&BZ2Decomp_Type);
}
