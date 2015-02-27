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

#ifndef PYSTON_RUNTIME_FILE_H
#define PYSTON_RUNTIME_FILE_H

#include "core/types.h"
#include "runtime/types.h"

namespace pyston {

class BoxedFile : public Box {
public:
    PyObject_HEAD FILE* f_fp;
    PyObject* f_name;
    PyObject* f_mode;
    int (*f_close)(FILE*);
    int f_softspace;    /* Flag used by 'print' command */
    int f_binary;       /* Flag which indicates whether the file is
                       open in binary (1) or text (0) mode */
    char* f_buf;        /* Allocated readahead buffer */
    char* f_bufend;     /* Points after last occupied position */
    char* f_bufptr;     /* Current buffer position */
    char* f_setbuf;     /* Buffer for setbuf(3) and setvbuf(3) */
    int f_univ_newline; /* Handle any newline convention */
    int f_newlinetypes; /* Types of newlines seen */
    int f_skipnextlf;   /* Skip next \n */
    PyObject* f_encoding;
    PyObject* f_errors;
    PyObject* weakreflist; /* List of weak references */
    int unlocked_count;    /* Num. currently running sections of code
                          using f_fp with the GIL released. */
    int readable;
    int writable;

    BoxedFile(FILE* f, std::string fname, const char* fmode, int (*close)(FILE*) = fclose)
        __attribute__((visibility("default")));

    DEFAULT_CLASS(file_cls);
};
}

#endif
