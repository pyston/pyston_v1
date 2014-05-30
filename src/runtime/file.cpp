// Copyright (c) 2014 Dropbox, Inc.
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

#include <cstring>
#include <sstream>

#include "codegen/compvars.h"
#include "core/common.h"
#include "core/stats.h"
#include "core/types.h"
#include "runtime/gc_runtime.h"
#include "runtime/objmodel.h"
#include "runtime/types.h"
#include "runtime/util.h"

namespace pyston {

Box* fileRepr(BoxedFile* self) {
    assert(self->cls == file_cls);
    RELEASE_ASSERT(0, "");
}

static Box* _fileRead(BoxedFile* self, i64 size) {
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

Box* fileRead1(BoxedFile* self) {
    assert(self->cls == file_cls);
    return _fileRead(self, -1);
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

Box* fileRead2(BoxedFile* self, Box* size) {
    assert(self->cls == file_cls);
    if (size->cls != int_cls) {
        fprintf(stderr, "TypeError: an integer is required\n");
        raiseExcHelper(TypeError, "");
    }
    return _fileRead(self, static_cast<BoxedInt*>(size)->n);
}

Box* fileWrite(BoxedFile* self, Box* val) {
    assert(self->cls == file_cls);

    if (self->closed) {
        fprintf(stderr, "IOError: file is closed\n");
        raiseExcHelper(IOError, "");
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

Box* fileNew2(BoxedClass* cls, Box* s) {
    assert(cls == file_cls);
    return open1(s);
}

Box* fileNew3(BoxedClass* cls, Box* s, Box* m) {
    assert(cls == file_cls);
    return open2(s, m);
}

void setupFile() {
    file_cls->giveAttr("__name__", boxStrConstant("file"));

    CLFunction* read = boxRTFunction((void*)fileRead1, NULL, 1, false);
    addRTFunction(read, (void*)fileRead2, NULL, 2, false);
    file_cls->giveAttr("read", new BoxedFunction(read));

    CLFunction* readline = boxRTFunction((void*)fileReadline1, STR, 1, false);
    file_cls->giveAttr("readline", new BoxedFunction(readline));

    file_cls->giveAttr("write", new BoxedFunction(boxRTFunction((void*)fileWrite, NULL, 2, false)));
    file_cls->giveAttr("close", new BoxedFunction(boxRTFunction((void*)fileClose, NULL, 1, false)));

    file_cls->giveAttr("__repr__", new BoxedFunction(boxRTFunction((void*)fileRepr, NULL, 1, false)));
    file_cls->giveAttr("__str__", file_cls->getattr("__repr__"));

    file_cls->giveAttr("__enter__", new BoxedFunction(boxRTFunction((void*)fileEnter, NULL, 1, false)));
    file_cls->giveAttr("__exit__", new BoxedFunction(boxRTFunction((void*)fileExit, NULL, 4, false)));

    CLFunction* __new__ = boxRTFunction((void*)fileNew2, NULL, 2, false);
    addRTFunction(__new__, (void*)fileNew3, NULL, 3, false);
    file_cls->giveAttr("__new__", new BoxedFunction(__new__));

    file_cls->freeze();
}

void teardownFile() {
}
}
