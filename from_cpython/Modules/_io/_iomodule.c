/*
    An implementation of the new I/O lib as defined by PEP 3116 - "New I/O"
    
    Classes defined here: UnsupportedOperation, BlockingIOError.
    Functions defined here: open().
    
    Mostly written by Amaury Forgeot d'Arc
*/

#define PY_SSIZE_T_CLEAN
#include "Python.h"
#include "structmember.h"
#include "_iomodule.h"

#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif /* HAVE_SYS_TYPES_H */

#ifdef HAVE_SYS_STAT_H
#include <sys/stat.h>
#endif /* HAVE_SYS_STAT_H */


/* Various interned strings */

PyObject *_PyIO_str_close;
PyObject *_PyIO_str_closed;
PyObject *_PyIO_str_decode;
PyObject *_PyIO_str_encode;
PyObject *_PyIO_str_fileno;
PyObject *_PyIO_str_flush;
PyObject *_PyIO_str_getstate;
PyObject *_PyIO_str_isatty;
PyObject *_PyIO_str_newlines;
PyObject *_PyIO_str_nl;
PyObject *_PyIO_str_read;
PyObject *_PyIO_str_read1;
PyObject *_PyIO_str_readable;
PyObject *_PyIO_str_readinto;
PyObject *_PyIO_str_readline;
PyObject *_PyIO_str_reset;
PyObject *_PyIO_str_seek;
PyObject *_PyIO_str_seekable;
PyObject *_PyIO_str_setstate;
PyObject *_PyIO_str_tell;
PyObject *_PyIO_str_truncate;
PyObject *_PyIO_str_writable;
PyObject *_PyIO_str_write;

PyObject *_PyIO_empty_str;
PyObject *_PyIO_empty_bytes;
PyObject *_PyIO_zero;


PyDoc_STRVAR(module_doc,
"The io module provides the Python interfaces to stream handling. The\n"
"builtin open function is defined in this module.\n"
"\n"
"At the top of the I/O hierarchy is the abstract base class IOBase. It\n"
"defines the basic interface to a stream. Note, however, that there is no\n"
"separation between reading and writing to streams; implementations are\n"
"allowed to raise an IOError if they do not support a given operation.\n"
"\n"
"Extending IOBase is RawIOBase which deals simply with the reading and\n"
"writing of raw bytes to a stream. FileIO subclasses RawIOBase to provide\n"
"an interface to OS files.\n"
"\n"
"BufferedIOBase deals with buffering on a raw byte stream (RawIOBase). Its\n"
"subclasses, BufferedWriter, BufferedReader, and BufferedRWPair buffer\n"
"streams that are readable, writable, and both respectively.\n"
"BufferedRandom provides a buffered interface to random access\n"
"streams. BytesIO is a simple stream of in-memory bytes.\n"
"\n"
"Another IOBase subclass, TextIOBase, deals with the encoding and decoding\n"
"of streams into text. TextIOWrapper, which extends it, is a buffered text\n"
"interface to a buffered raw stream (`BufferedIOBase`). Finally, StringIO\n"
"is a in-memory stream for text.\n"
"\n"
"Argument names are not part of the specification, and only the arguments\n"
"of open() are intended to be used as keyword arguments.\n"
"\n"
"data:\n"
"\n"
"DEFAULT_BUFFER_SIZE\n"
"\n"
"   An int containing the default buffer size used by the module's buffered\n"
"   I/O classes. open() uses the file's blksize (as obtained by os.stat) if\n"
"   possible.\n"
    );


/*
 * BlockingIOError extends IOError
 */

static int
blockingioerror_init(PyBlockingIOErrorObject *self, PyObject *args,
                     PyObject *kwds)
{
    PyObject *myerrno = NULL, *strerror = NULL;
    PyObject *baseargs = NULL;
    Py_ssize_t written = 0;

    assert(PyTuple_Check(args));

    self->written = 0;
    if (!PyArg_ParseTuple(args, "OO|n:BlockingIOError",
                          &myerrno, &strerror, &written))
        return -1;

    baseargs = PyTuple_Pack(2, myerrno, strerror);
    if (baseargs == NULL)
        return -1;
    /* This will take care of initializing of myerrno and strerror members */
    if (((PyTypeObject *)PyExc_IOError)->tp_init(
                (PyObject *)self, baseargs, kwds) == -1) {
        Py_DECREF(baseargs);
        return -1;
    }
    Py_DECREF(baseargs);

    self->written = written;
    return 0;
}

static PyMemberDef blockingioerror_members[] = {
    {"characters_written", T_PYSSIZET, offsetof(PyBlockingIOErrorObject, written), 0},
    {NULL}  /* Sentinel */
};

static PyTypeObject _PyExc_BlockingIOError = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "BlockingIOError", /*tp_name*/
    sizeof(PyBlockingIOErrorObject), /*tp_basicsize*/
    0,                          /*tp_itemsize*/
    0,                          /*tp_dealloc*/
    0,                          /*tp_print*/
    0,                          /*tp_getattr*/
    0,                          /*tp_setattr*/
    0,                          /*tp_compare */
    0,                          /*tp_repr*/
    0,                          /*tp_as_number*/
    0,                          /*tp_as_sequence*/
    0,                          /*tp_as_mapping*/
    0,                          /*tp_hash */
    0,                          /*tp_call*/
    0,                          /*tp_str*/
    0,                          /*tp_getattro*/
    0,                          /*tp_setattro*/
    0,                          /*tp_as_buffer*/
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE, /*tp_flags*/
    PyDoc_STR("Exception raised when I/O would block "
              "on a non-blocking I/O stream"), /* tp_doc */
    0,                          /* tp_traverse */
    0,                          /* tp_clear */
    0,                          /* tp_richcompare */
    0,                          /* tp_weaklistoffset */
    0,                          /* tp_iter */
    0,                          /* tp_iternext */
    0,                          /* tp_methods */
    blockingioerror_members,    /* tp_members */
    0,                          /* tp_getset */
    0,                          /* tp_base */
    0,                          /* tp_dict */
    0,                          /* tp_descr_get */
    0,                          /* tp_descr_set */
    0,                          /* tp_dictoffset */
    (initproc)blockingioerror_init, /* tp_init */
    0,                          /* tp_alloc */
    0,                          /* tp_new */
};
PyObject *PyExc_BlockingIOError = (PyObject *)&_PyExc_BlockingIOError;


/*
 * The main open() function
 */
PyDoc_STRVAR(open_doc,
"Open file and return a stream.  Raise IOError upon failure.\n"
"\n"
"file is either a text or byte string giving the name (and the path\n"
"if the file isn't in the current working directory) of the file to\n"
"be opened or an integer file descriptor of the file to be\n"
"wrapped. (If a file descriptor is given, it is closed when the\n"
"returned I/O object is closed, unless closefd is set to False.)\n"
"\n"
"mode is an optional string that specifies the mode in which the file\n"
"is opened. It defaults to 'r' which means open for reading in text\n"
"mode.  Other common values are 'w' for writing (truncating the file if\n"
"it already exists), and 'a' for appending (which on some Unix systems,\n"
"means that all writes append to the end of the file regardless of the\n"
"current seek position). In text mode, if encoding is not specified the\n"
"encoding used is platform dependent. (For reading and writing raw\n"
"bytes use binary mode and leave encoding unspecified.) The available\n"
"modes are:\n"
"\n"
"========= ===============================================================\n"
"Character Meaning\n"
"--------- ---------------------------------------------------------------\n"
"'r'       open for reading (default)\n"
"'w'       open for writing, truncating the file first\n"
"'a'       open for writing, appending to the end of the file if it exists\n"
"'b'       binary mode\n"
"'t'       text mode (default)\n"
"'+'       open a disk file for updating (reading and writing)\n"
"'U'       universal newline mode (for backwards compatibility; unneeded\n"
"          for new code)\n"
"========= ===============================================================\n"
"\n"
"The default mode is 'rt' (open for reading text). For binary random\n"
"access, the mode 'w+b' opens and truncates the file to 0 bytes, while\n"
"'r+b' opens the file without truncation.\n"
"\n"
"Python distinguishes between files opened in binary and text modes,\n"
"even when the underlying operating system doesn't. Files opened in\n"
"binary mode (appending 'b' to the mode argument) return contents as\n"
"bytes objects without any decoding. In text mode (the default, or when\n"
"'t' is appended to the mode argument), the contents of the file are\n"
"returned as strings, the bytes having been first decoded using a\n"
"platform-dependent encoding or using the specified encoding if given.\n"
"\n"
"buffering is an optional integer used to set the buffering policy.\n"
"Pass 0 to switch buffering off (only allowed in binary mode), 1 to select\n"
"line buffering (only usable in text mode), and an integer > 1 to indicate\n"
"the size of a fixed-size chunk buffer.  When no buffering argument is\n"
"given, the default buffering policy works as follows:\n"
"\n"
"* Binary files are buffered in fixed-size chunks; the size of the buffer\n"
"  is chosen using a heuristic trying to determine the underlying device's\n"
"  \"block size\" and falling back on `io.DEFAULT_BUFFER_SIZE`.\n"
"  On many systems, the buffer will typically be 4096 or 8192 bytes long.\n"
"\n"
"* \"Interactive\" text files (files for which isatty() returns True)\n"
"  use line buffering.  Other text files use the policy described above\n"
"  for binary files.\n"
"\n"
"encoding is the name of the encoding used to decode or encode the\n"
"file. This should only be used in text mode. The default encoding is\n"
"platform dependent, but any encoding supported by Python can be\n"
"passed.  See the codecs module for the list of supported encodings.\n"
"\n"
"errors is an optional string that specifies how encoding errors are to\n"
"be handled---this argument should not be used in binary mode. Pass\n"
"'strict' to raise a ValueError exception if there is an encoding error\n"
"(the default of None has the same effect), or pass 'ignore' to ignore\n"
"errors. (Note that ignoring encoding errors can lead to data loss.)\n"
"See the documentation for codecs.register for a list of the permitted\n"
"encoding error strings.\n"
"\n"
"newline controls how universal newlines works (it only applies to text\n"
"mode). It can be None, '', '\\n', '\\r', and '\\r\\n'.  It works as\n"
"follows:\n"
"\n"
"* On input, if newline is None, universal newlines mode is\n"
"  enabled. Lines in the input can end in '\\n', '\\r', or '\\r\\n', and\n"
"  these are translated into '\\n' before being returned to the\n"
"  caller. If it is '', universal newline mode is enabled, but line\n"
"  endings are returned to the caller untranslated. If it has any of\n"
"  the other legal values, input lines are only terminated by the given\n"
"  string, and the line ending is returned to the caller untranslated.\n"
"\n"
"* On output, if newline is None, any '\\n' characters written are\n"
"  translated to the system default line separator, os.linesep. If\n"
"  newline is '', no translation takes place. If newline is any of the\n"
"  other legal values, any '\\n' characters written are translated to\n"
"  the given string.\n"
"\n"
"If closefd is False, the underlying file descriptor will be kept open\n"
"when the file is closed. This does not work when a file name is given\n"
"and must be True in that case.\n"
"\n"
"open() returns a file object whose type depends on the mode, and\n"
"through which the standard file operations such as reading and writing\n"
"are performed. When open() is used to open a file in a text mode ('w',\n"
"'r', 'wt', 'rt', etc.), it returns a TextIOWrapper. When used to open\n"
"a file in a binary mode, the returned class varies: in read binary\n"
"mode, it returns a BufferedReader; in write binary and append binary\n"
"modes, it returns a BufferedWriter, and in read/write mode, it returns\n"
"a BufferedRandom.\n"
"\n"
"It is also possible to use a string or bytearray as a file for both\n"
"reading and writing. For strings StringIO can be used like a file\n"
"opened in a text mode, and for bytes a BytesIO can be used like a file\n"
"opened in a binary mode.\n"
    );

static PyObject *
io_open(PyObject *self, PyObject *args, PyObject *kwds)
{
    char *kwlist[] = {"file", "mode", "buffering",
                      "encoding", "errors", "newline",
                      "closefd", NULL};
    PyObject *file;
    char *mode = "r";
    int buffering = -1, closefd = 1;
    char *encoding = NULL, *errors = NULL, *newline = NULL;
    unsigned i;

    int reading = 0, writing = 0, appending = 0, updating = 0;
    int text = 0, binary = 0, universal = 0;

    char rawmode[5], *m;
    int line_buffering;
    long isatty;

    PyObject *raw, *modeobj = NULL, *buffer = NULL, *wrapper = NULL;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O|sizzzi:open", kwlist,
                                     &file, &mode, &buffering,
                                     &encoding, &errors, &newline,
                                     &closefd)) {
        return NULL;
    }

    if (!PyUnicode_Check(file) &&
	!PyBytes_Check(file) &&
	!PyNumber_Check(file)) {
        PyObject *repr = PyObject_Repr(file);
        if (repr != NULL) {
            PyErr_Format(PyExc_TypeError, "invalid file: %s",
                         PyString_AS_STRING(repr));
            Py_DECREF(repr);
        }
        return NULL;
    }

    /* Decode mode */
    for (i = 0; i < strlen(mode); i++) {
        char c = mode[i];

        switch (c) {
        case 'r':
            reading = 1;
            break;
        case 'w':
            writing = 1;
            break;
        case 'a':
            appending = 1;
            break;
        case '+':
            updating = 1;
            break;
        case 't':
            text = 1;
            break;
        case 'b':
            binary = 1;
            break;
        case 'U':
            universal = 1;
            reading = 1;
            break;
        default:
            goto invalid_mode;
        }

        /* c must not be duplicated */
        if (strchr(mode+i+1, c)) {
          invalid_mode:
            PyErr_Format(PyExc_ValueError, "invalid mode: '%s'", mode);
            return NULL;
        }

    }

    m = rawmode;
    if (reading)   *(m++) = 'r';
    if (writing)   *(m++) = 'w';
    if (appending) *(m++) = 'a';
    if (updating)  *(m++) = '+';
    *m = '\0';

    /* Parameters validation */
    if (universal) {
        if (writing || appending) {
            PyErr_SetString(PyExc_ValueError,
                            "can't use U and writing mode at once");
            return NULL;
        }
        reading = 1;
    }

    if (text && binary) {
        PyErr_SetString(PyExc_ValueError,
                        "can't have text and binary mode at once");
        return NULL;
    }

    if (reading + writing + appending > 1) {
        PyErr_SetString(PyExc_ValueError,
                        "must have exactly one of read/write/append mode");
        return NULL;
    }

    if (binary && encoding != NULL) {
        PyErr_SetString(PyExc_ValueError,
                        "binary mode doesn't take an encoding argument");
        return NULL;
    }

    if (binary && errors != NULL) {
        PyErr_SetString(PyExc_ValueError,
                        "binary mode doesn't take an errors argument");
        return NULL;
    }

    if (binary && newline != NULL) {
        PyErr_SetString(PyExc_ValueError,
                        "binary mode doesn't take a newline argument");
        return NULL;
    }

    /* Create the Raw file stream */
    raw = PyObject_CallFunction((PyObject *)&PyFileIO_Type,
				"Osi", file, rawmode, closefd);
    if (raw == NULL)
        return NULL;

    modeobj = PyUnicode_FromString(mode);
    if (modeobj == NULL)
        goto error;

    /* buffering */
    {
        PyObject *res = PyObject_CallMethod(raw, "isatty", NULL);
        if (res == NULL)
            goto error;
        isatty = PyLong_AsLong(res);
        Py_DECREF(res);
        if (isatty == -1 && PyErr_Occurred())
            goto error;
    }

    if (buffering == 1 || (buffering < 0 && isatty)) {
        buffering = -1;
        line_buffering = 1;
    }
    else
        line_buffering = 0;

    if (buffering < 0) {
        buffering = DEFAULT_BUFFER_SIZE;
#ifdef HAVE_STRUCT_STAT_ST_BLKSIZE
        {
            struct stat st;
            int fileno;
            PyObject *res = PyObject_CallMethod(raw, "fileno", NULL);
            if (res == NULL)
                goto error;

            fileno = _PyInt_AsInt(res);
            Py_DECREF(res);
            if (fileno == -1 && PyErr_Occurred())
                goto error;

            if (fstat(fileno, &st) >= 0 && st.st_blksize > 1)
                buffering = st.st_blksize;
        }
#endif
    }
    if (buffering < 0) {
        PyErr_SetString(PyExc_ValueError,
                        "invalid buffering size");
        goto error;
    }

    /* if not buffering, returns the raw file object */
    if (buffering == 0) {
        if (!binary) {
            PyErr_SetString(PyExc_ValueError,
                            "can't have unbuffered text I/O");
            goto error;
        }

        Py_DECREF(modeobj);
        return raw;
    }

    /* wraps into a buffered file */
    {
        PyObject *Buffered_class;

        if (updating)
            Buffered_class = (PyObject *)&PyBufferedRandom_Type;
        else if (writing || appending)
            Buffered_class = (PyObject *)&PyBufferedWriter_Type;
        else if (reading)
            Buffered_class = (PyObject *)&PyBufferedReader_Type;
        else {
            PyErr_Format(PyExc_ValueError,
                         "unknown mode: '%s'", mode);
            goto error;
        }

        buffer = PyObject_CallFunction(Buffered_class, "Oi", raw, buffering);
    }
    Py_CLEAR(raw);
    if (buffer == NULL)
        goto error;


    /* if binary, returns the buffered file */
    if (binary) {
        Py_DECREF(modeobj);
        return buffer;
    }

    /* wraps into a TextIOWrapper */
    wrapper = PyObject_CallFunction((PyObject *)&PyTextIOWrapper_Type,
				    "Osssi",
				    buffer,
				    encoding, errors, newline,
				    line_buffering);
    Py_CLEAR(buffer);
    if (wrapper == NULL)
        goto error;

    if (PyObject_SetAttrString(wrapper, "mode", modeobj) < 0)
        goto error;
    Py_DECREF(modeobj);
    return wrapper;

  error:
    Py_XDECREF(raw);
    Py_XDECREF(modeobj);
    Py_XDECREF(buffer);
    Py_XDECREF(wrapper);
    return NULL;
}

/*
 * Private helpers for the io module.
 */

Py_off_t
PyNumber_AsOff_t(PyObject *item, PyObject *err)
{
    Py_off_t result;
    PyObject *runerr;
    PyObject *value = PyNumber_Index(item);
    if (value == NULL)
        return -1;

    if (PyInt_Check(value)) {
        /* We assume a long always fits in a Py_off_t... */
        result = (Py_off_t) PyInt_AS_LONG(value);
        goto finish;
    }

    /* We're done if PyLong_AsSsize_t() returns without error. */
    result = PyLong_AsOff_t(value);
    if (result != -1 || !(runerr = PyErr_Occurred()))
        goto finish;

    /* Error handling code -- only manage OverflowError differently */
    if (!PyErr_GivenExceptionMatches(runerr, PyExc_OverflowError))
        goto finish;

    PyErr_Clear();
    /* If no error-handling desired then the default clipping
       is sufficient.
     */
    if (!err) {
        assert(PyLong_Check(value));
        /* Whether or not it is less than or equal to
           zero is determined by the sign of ob_size
        */
        if (_PyLong_Sign(value) < 0)
            result = PY_OFF_T_MIN;
        else
            result = PY_OFF_T_MAX;
    }
    else {
        /* Otherwise replace the error with caller's error object. */
        PyErr_Format(err,
                     "cannot fit '%.200s' into an offset-sized integer",
                     item->ob_type->tp_name);
    }

 finish:
    Py_DECREF(value);
    return result;
}


/* Basically the "n" format code with the ability to turn None into -1. */
int 
_PyIO_ConvertSsize_t(PyObject *obj, void *result) {
    Py_ssize_t limit;
    if (obj == Py_None) {
        limit = -1;
    }
    else if (PyNumber_Check(obj)) {
        limit = PyNumber_AsSsize_t(obj, PyExc_OverflowError);
        if (limit == -1 && PyErr_Occurred())
            return 0;
    }
    else {
        PyErr_Format(PyExc_TypeError,
                     "integer argument expected, got '%.200s'",
                     Py_TYPE(obj)->tp_name);
        return 0;
    }
    *((Py_ssize_t *)result) = limit;
    return 1;
}


/*
 * Module definition
 */

PyObject *_PyIO_os_module = NULL;
PyObject *_PyIO_locale_module = NULL;
PyObject *_PyIO_unsupported_operation = NULL;

static PyMethodDef module_methods[] = {
    {"open", (PyCFunction)io_open, METH_VARARGS|METH_KEYWORDS, open_doc},
    {NULL, NULL}
};

PyMODINIT_FUNC
init_io(void)
{
    PyObject *m = Py_InitModule4("_io", module_methods,
                                 module_doc, NULL, PYTHON_API_VERSION);
    if (m == NULL)
        return;

    // Pyston change: we don't support importing os during init, but it looks like it works without it...
#if 0
    /* put os in the module state */
    _PyIO_os_module = PyImport_ImportModule("os");
    if (_PyIO_os_module == NULL)
        goto fail;
#endif

#define ADD_TYPE(type, name) \
    if (PyType_Ready(type) < 0) \
        goto fail; \
    Py_INCREF(type); \
    if (PyModule_AddObject(m, name, (PyObject *)type) < 0) {  \
        Py_DECREF(type); \
        goto fail; \
    }

    /* DEFAULT_BUFFER_SIZE */
    if (PyModule_AddIntMacro(m, DEFAULT_BUFFER_SIZE) < 0)
        goto fail;

    /* UnsupportedOperation inherits from ValueError and IOError */
    // Pyston change: during init we have to supply the module name by ourself.
    _PyIO_unsupported_operation = PyObject_CallFunction((PyObject *)&PyType_Type, "s(OO){s:s}",
          "UnsupportedOperation", PyExc_ValueError, PyExc_IOError, "__module__", "io");
    // _PyIO_unsupported_operation = PyObject_CallFunction(
    //    (PyObject *)&PyType_Type, "s(OO){}",
    //    "UnsupportedOperation", PyExc_ValueError, PyExc_IOError);

    if (_PyIO_unsupported_operation == NULL)
        goto fail;
    Py_INCREF(_PyIO_unsupported_operation);
    if (PyModule_AddObject(m, "UnsupportedOperation",
                           _PyIO_unsupported_operation) < 0)
        goto fail;

    /* BlockingIOError */
    _PyExc_BlockingIOError.tp_base = (PyTypeObject *) PyExc_IOError;
    ADD_TYPE(&_PyExc_BlockingIOError, "BlockingIOError");

    /* Concrete base types of the IO ABCs.
       (the ABCs themselves are declared through inheritance in io.py)
    */
    ADD_TYPE(&PyIOBase_Type, "_IOBase");
    ADD_TYPE(&PyRawIOBase_Type, "_RawIOBase");
    ADD_TYPE(&PyBufferedIOBase_Type, "_BufferedIOBase");
    ADD_TYPE(&PyTextIOBase_Type, "_TextIOBase");

    /* Implementation of concrete IO objects. */
    /* FileIO */
    PyFileIO_Type.tp_base = &PyRawIOBase_Type;
    ADD_TYPE(&PyFileIO_Type, "FileIO");

    /* BytesIO */
    PyBytesIO_Type.tp_base = &PyBufferedIOBase_Type;
    ADD_TYPE(&PyBytesIO_Type, "BytesIO");

    /* StringIO */
    PyStringIO_Type.tp_base = &PyTextIOBase_Type;
    ADD_TYPE(&PyStringIO_Type, "StringIO");

    /* BufferedReader */
    PyBufferedReader_Type.tp_base = &PyBufferedIOBase_Type;
    ADD_TYPE(&PyBufferedReader_Type, "BufferedReader");

    /* BufferedWriter */
    PyBufferedWriter_Type.tp_base = &PyBufferedIOBase_Type;
    ADD_TYPE(&PyBufferedWriter_Type, "BufferedWriter");

    /* BufferedRWPair */
    PyBufferedRWPair_Type.tp_base = &PyBufferedIOBase_Type;
    ADD_TYPE(&PyBufferedRWPair_Type, "BufferedRWPair");

    /* BufferedRandom */
    PyBufferedRandom_Type.tp_base = &PyBufferedIOBase_Type;
    ADD_TYPE(&PyBufferedRandom_Type, "BufferedRandom");

    /* TextIOWrapper */
    PyTextIOWrapper_Type.tp_base = &PyTextIOBase_Type;
    ADD_TYPE(&PyTextIOWrapper_Type, "TextIOWrapper");

    /* IncrementalNewlineDecoder */
    ADD_TYPE(&PyIncrementalNewlineDecoder_Type, "IncrementalNewlineDecoder");

    // Pyston change: register with gc
    // TODO: automatically register static variables as roots
    /* Interned strings */
    // Pyston change: these are no longered interned since the interned-ness wasn't being relied on,
    // and we don't yet support real on-demand interning.
    if (!(_PyIO_str_close = PyGC_AddRoot(PyString_FromString("close"))))
        goto fail;
    if (!(_PyIO_str_closed = PyGC_AddRoot(PyString_FromString("closed"))))
        goto fail;
    if (!(_PyIO_str_decode = PyGC_AddRoot(PyString_FromString("decode"))))
        goto fail;
    if (!(_PyIO_str_encode = PyGC_AddRoot(PyString_FromString("encode"))))
        goto fail;
    if (!(_PyIO_str_fileno = PyGC_AddRoot(PyString_FromString("fileno"))))
        goto fail;
    if (!(_PyIO_str_flush = PyGC_AddRoot(PyString_FromString("flush"))))
        goto fail;
    if (!(_PyIO_str_getstate = PyGC_AddRoot(PyString_FromString("getstate"))))
        goto fail;
    if (!(_PyIO_str_isatty = PyGC_AddRoot(PyString_FromString("isatty"))))
        goto fail;
    if (!(_PyIO_str_newlines = PyGC_AddRoot(PyString_FromString("newlines"))))
        goto fail;
    if (!(_PyIO_str_nl = PyGC_AddRoot(PyString_FromString("\n"))))
        goto fail;
    if (!(_PyIO_str_read = PyGC_AddRoot(PyString_FromString("read"))))
        goto fail;
    if (!(_PyIO_str_read1 = PyGC_AddRoot(PyString_FromString("read1"))))
        goto fail;
    if (!(_PyIO_str_readable = PyGC_AddRoot(PyString_FromString("readable"))))
        goto fail;
    if (!(_PyIO_str_readinto = PyGC_AddRoot(PyString_FromString("readinto"))))
        goto fail;
    if (!(_PyIO_str_readline = PyGC_AddRoot(PyString_FromString("readline"))))
        goto fail;
    if (!(_PyIO_str_reset = PyGC_AddRoot(PyString_FromString("reset"))))
        goto fail;
    if (!(_PyIO_str_seek = PyGC_AddRoot(PyString_FromString("seek"))))
        goto fail;
    if (!(_PyIO_str_seekable = PyGC_AddRoot(PyString_FromString("seekable"))))
        goto fail;
    if (!(_PyIO_str_setstate = PyGC_AddRoot(PyString_FromString("setstate"))))
        goto fail;
    if (!(_PyIO_str_tell = PyGC_AddRoot(PyString_FromString("tell"))))
        goto fail;
    if (!(_PyIO_str_truncate = PyGC_AddRoot(PyString_FromString("truncate"))))
        goto fail;
    if (!(_PyIO_str_write = PyGC_AddRoot(PyString_FromString("write"))))
        goto fail;
    if (!(_PyIO_str_writable = PyGC_AddRoot(PyString_FromString("writable"))))
        goto fail;
    
    if (!(_PyIO_empty_str = PyGC_AddRoot(PyUnicode_FromStringAndSize(NULL, 0))))
        goto fail;
    if (!(_PyIO_empty_bytes = PyGC_AddRoot(PyBytes_FromStringAndSize(NULL, 0))))
        goto fail;
    if (!(_PyIO_zero = PyGC_AddRoot(PyLong_FromLong(0L))))
        goto fail;

    return;

  fail:
    Py_CLEAR(_PyIO_os_module);
    Py_CLEAR(_PyIO_unsupported_operation);
    Py_DECREF(m);
}
