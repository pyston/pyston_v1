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

Box* fileRepr(BoxedFile* self) {
    assert(self->cls == file_cls);

    void* addr = static_cast<void*>(self->f);
    std::ostringstream repr;

    repr << "<" << (self->closed ? "closed" : "open") << " file '" << self->fname << "', ";
    repr << "mode '" << self->fmode << "' at " << addr << ">";

    return boxString(repr.str());
}

Box* fileRead(BoxedFile* self, Box* _size) {
    assert(self->cls == file_cls);
    if (_size->cls != int_cls) {
        fprintf(stderr, "TypeError: an integer is required\n");
        raiseExcHelper(TypeError, "");
    }
    int64_t size = static_cast<BoxedInt*>(_size)->n;

    if (self->closed) {
        fprintf(stderr, "IOError: file not open for reading\n");
        raiseExcHelper(IOError, "");
    }

    std::ostringstream os("");

    if (size < 0)
        size = 1L << 60;

    i64 read = 0;
    while (read < size) {
        const int BUF_SIZE = 1024;
        char buf[BUF_SIZE];
        size_t more_read = fread(buf, 1, std::min((i64)BUF_SIZE, size - read), self->f);
        if (more_read == 0) {
            ASSERT(!ferror(self->f), "%d", ferror(self->f));
            break;
        }

        read += more_read;
        // this is probably inefficient:
        os << std::string(buf, more_read);
    }
    return boxString(os.str());
}

Box* fileReadline1(BoxedFile* self) {
    assert(self->cls == file_cls);

    std::ostringstream os("");

    while (true) {
        char c;
        int nread = fread(&c, 1, 1, self->f);
        if (nread == 0)
            break;
        os << c;

        if (c == '\n')
            break;
    }
    return boxString(os.str());
}

Box* fileWrite(BoxedFile* self, Box* val) {
    assert(self->cls == file_cls);

    if (self->closed) {
        raiseExcHelper(IOError, "file is closed");
    }


    if (val->cls == str_cls) {
        const std::string& s = static_cast<BoxedString*>(val)->s;

        size_t size = s.size();
        size_t written = 0;
        while (written < size) {
            // const int BUF_SIZE = 1024;
            // char buf[BUF_SIZE];
            // int to_write = std::min(BUF_SIZE, size - written);
            // memcpy(buf, s.c_str() + written, to_write);
            // size_t new_written = fwrite(buf, 1, to_write, self->f);

            size_t new_written = fwrite(s.c_str() + written, 1, size - written, self->f);

            if (!new_written) {
                int error = ferror(self->f);
                fprintf(stderr, "IOError %d\n", error);
                raiseExcHelper(IOError, "");
            }

            written += new_written;
        }

        return None;
    } else {
        fprintf(stderr, "TypeError: expected a character buffer object\n");
        raiseExcHelper(TypeError, "");
    }
}

Box* fileFlush(BoxedFile* self) {
    RELEASE_ASSERT(self->cls == file_cls, "");

    if (self->closed)
        raiseExcHelper(IOError, "file is closed");

    fflush(self->f);
    return None;
}

Box* fileClose(BoxedFile* self) {
    assert(self->cls == file_cls);
    if (self->closed) {
        fprintf(stderr, "IOError: file is closed\n");
        raiseExcHelper(IOError, "");
    }

    fclose(self->f);
    self->closed = true;

    return None;
}

Box* fileEnter(BoxedFile* self) {
    assert(self->cls == file_cls);
    return self;
}

Box* fileExit(BoxedFile* self, Box* exc_type, Box* exc_val, Box** args) {
    Box* exc_tb = args[0];
    assert(self->cls == file_cls);
    assert(exc_type == None);
    assert(exc_val == None);
    assert(exc_tb == None);

    return fileClose(self);
}

Box* fileNew(BoxedClass* cls, Box* s, Box* m) {
    assert(cls == file_cls);
    return open(s, m);
}

Box* fileIterNext(BoxedFile* s) {
    return fileReadline1(s);
}

bool fileEof(BoxedFile* self) {
    char ch = fgetc(self->f);
    ungetc(ch, self->f);
    return feof(self->f);
}

Box* fileIterHasNext(Box* s) {
    assert(s->cls == file_cls);
    BoxedFile* self = static_cast<BoxedFile*>(s);
    return boxBool(!fileEof(self));
}

extern "C" void PyFile_SetFP(PyObject* _f, FILE* fp) noexcept {
    assert(_f->cls == file_cls);
    BoxedFile* f = static_cast<BoxedFile*>(_f);
    assert(f->f == NULL);
    f->f = fp;
}

extern "C" PyObject* PyFile_FromFile(FILE* fp, char* name, char* mode, int (*close)(FILE*)) noexcept {
    RELEASE_ASSERT(close == fclose, "unsupported");
    return new BoxedFile(fp, name, mode);
}

extern "C" FILE* PyFile_AsFile(PyObject* f) noexcept {
    if (!f || !PyFile_Check(f))
        return NULL;

    return static_cast<BoxedFile*>(f)->f;
}

extern "C" int PyFile_WriteObject(PyObject* v, PyObject* f, int flags) noexcept {
    if (f->cls != file_cls || v->cls != str_cls || flags != Py_PRINT_RAW)
        Py_FatalError("unimplemented");
    try {
        Box* r = fileWrite(static_cast<BoxedFile*>(f), v);
        assert(r == None);
        return 0;
    } catch (ExcInfo e) {
        setCAPIException(e);
        return -1;
    }
}

#define FILE_BEGIN_ALLOW_THREADS(fobj)                                                                                 \
    {                                                                                                                  \
    /*fobj->unlocked_count++;*/                                                                                        \
    Py_BEGIN_ALLOW_THREADS

#define FILE_END_ALLOW_THREADS(fobj)                                                                                   \
    Py_END_ALLOW_THREADS                                                                                               \
    /*fobj->unlocked_count--;*/                                                                                        \
    /*assert(fobj->unlocked_count >= 0);*/                                                                             \
    }

static PyObject* err_closed(void) noexcept {
    PyErr_SetString(PyExc_ValueError, "I/O operation on closed file");
    return NULL;
}

extern "C" int PyFile_WriteString(const char* s, PyObject* f) noexcept {
    if (f == NULL) {
        /* Should be caused by a pre-existing error */
        if (!PyErr_Occurred())
            PyErr_SetString(PyExc_SystemError, "null file for PyFile_WriteString");
        return -1;
    } else if (PyFile_Check(f)) {
        PyFileObject* fobj = (PyFileObject*)f;
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
    if (bufsize >= 0) {
        if (bufsize == 0) {
            setvbuf(static_cast<BoxedFile*>(f)->f, NULL, _IONBF, 0);
        } else {
            Py_FatalError("unimplemented");
        }
    }
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
        abort();
    }
}

void setupFile() {
    file_cls->giveAttr("__name__", boxStrConstant("file"));

    file_cls->giveAttr("read",
                       new BoxedFunction(boxRTFunction((void*)fileRead, STR, 2, 1, false, false), { boxInt(-1) }));

    CLFunction* readline = boxRTFunction((void*)fileReadline1, STR, 1);
    file_cls->giveAttr("readline", new BoxedFunction(readline));

    file_cls->giveAttr("flush", new BoxedFunction(boxRTFunction((void*)fileFlush, NONE, 1)));
    file_cls->giveAttr("write", new BoxedFunction(boxRTFunction((void*)fileWrite, NONE, 2)));
    file_cls->giveAttr("close", new BoxedFunction(boxRTFunction((void*)fileClose, NONE, 1)));

    file_cls->giveAttr("__repr__", new BoxedFunction(boxRTFunction((void*)fileRepr, STR, 1)));

    file_cls->giveAttr("__enter__", new BoxedFunction(boxRTFunction((void*)fileEnter, typeFromClass(file_cls), 1)));
    file_cls->giveAttr("__exit__", new BoxedFunction(boxRTFunction((void*)fileExit, UNKNOWN, 4)));

    file_cls->giveAttr("__iter__", file_cls->getattr("__enter__"));
    file_cls->giveAttr("__hasnext__", new BoxedFunction(boxRTFunction((void*)fileIterHasNext, BOXED_BOOL, 1)));
    file_cls->giveAttr("next", new BoxedFunction(boxRTFunction((void*)fileIterNext, STR, 1)));

    file_cls->giveAttr("softspace",
                       new BoxedMemberDescriptor(BoxedMemberDescriptor::BYTE, offsetof(BoxedFile, softspace)));

    file_cls->giveAttr("__new__", new BoxedFunction(boxRTFunction((void*)fileNew, UNKNOWN, 3, 1, false, false),
                                                    { boxStrConstant("r") }));

    file_cls->freeze();
}

void teardownFile() {
}
}
