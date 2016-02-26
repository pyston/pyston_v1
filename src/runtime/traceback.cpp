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

#include "runtime/traceback.h"

#include <algorithm>
#include <cstring>
#include <sstream>

#include "capi/types.h"
#include "core/ast.h"
#include "core/common.h"
#include "core/stats.h"
#include "core/types.h"
#include "runtime/inline/list.h"
#include "runtime/list.h"
#include "runtime/objmodel.h"
#include "runtime/types.h"
#include "runtime/util.h"

namespace pyston {

extern "C" {
BoxedClass* traceback_cls;
}

void printTraceback(Box* b) {
    if (b == None)
        return;
    assert(b->cls == traceback_cls);

    BoxedTraceback* tb = static_cast<BoxedTraceback*>(b);

    fprintf(stderr, "Traceback (most recent call last):\n");

    for (; tb && tb != None; tb = static_cast<BoxedTraceback*>(tb->tb_next)) {
        auto& line = tb->line;
        fprintf(stderr, "  File \"%s\", line %d, in %s\n", line.file->c_str(), line.line, line.func->c_str());

        if (line.line < 0)
            continue;

        FILE* f = fopen(line.file->c_str(), "r");
        if (f) {
            assert(line.line < 10000000 && "Refusing to try to seek that many lines forward");
            for (int i = 1; i < line.line; i++) {
                char* buf = NULL;
                size_t size;
                size_t r = getline(&buf, &size, f);
                if (r != -1)
                    free(buf);
            }
            char* buf = NULL;
            size_t size;
            size_t r = getline(&buf, &size, f);
            if (r != -1) {
                while (buf[r - 1] == '\n' or buf[r - 1] == '\r')
                    r--;

                char* ptr = buf;
                while (*ptr == ' ' || *ptr == '\t') {
                    ptr++;
                    r--;
                }

                fprintf(stderr, "    %.*s\n", (int)r, ptr);
                free(buf);
            }
            fclose(f);
        }
    }
}

void BoxedTraceback::dealloc(Box* b) noexcept {
    BoxedTraceback* self = static_cast<BoxedTraceback*>(b);

    Py_DECREF(self->tb_next);
    Py_DECREF(self->line.file);
    Py_DECREF(self->line.func);

    Py_DECREF(self->tb_frame);

    PyObject_GC_Del(b);
}

int BoxedTraceback::traverse(Box* self, visitproc visit, void *arg) noexcept {
    BoxedTraceback* tb = static_cast<BoxedTraceback*>(self);

    Py_VISIT(tb->tb_next);
    Py_VISIT(tb->tb_frame);
    Py_VISIT(tb->line.file);
    Py_VISIT(tb->line.func);

    return 0;
}

int BoxedTraceback::clear(Box* self) noexcept {
    abort();
}

void BoxedTraceback::here(LineInfo lineInfo, Box** tb, Box* frame) {
    Box* old_tb = *tb;
    *tb = new BoxedTraceback(std::move(lineInfo), *tb, frame);
    Py_DECREF(old_tb);
}

extern "C" int _Py_DisplaySourceLine(PyObject* f, const char* filename, int lineno, int indent) noexcept {
    RELEASE_ASSERT(0, "Not implemented.");
}

void setupTraceback() {
    traceback_cls = BoxedClass::create(type_cls, object_cls, 0, 0, sizeof(BoxedTraceback), false, "traceback", true,
                                       (destructor)BoxedTraceback::dealloc, NULL, true,
                                       (traverseproc)BoxedTraceback::traverse, (inquiry)BoxedTraceback::clear);

    /*
     * Currently not supported.
    traceback_cls->giveAttr("tb_lasti", new (pyston_getset_cls) BoxedGetsetDescriptor(traceback_tb_lasti, NULL, NULL));
    */

    traceback_cls->giveAttr(
        "tb_frame", new BoxedMemberDescriptor(BoxedMemberDescriptor::OBJECT, offsetof(BoxedTraceback, tb_frame)));
    traceback_cls->giveAttr(
        "tb_next", new BoxedMemberDescriptor(BoxedMemberDescriptor::OBJECT, offsetof(BoxedTraceback, tb_next)));
    traceback_cls->giveAttr("tb_lineno",
                            new BoxedMemberDescriptor(BoxedMemberDescriptor::INT,
                                                      offsetof(BoxedTraceback, line) + offsetof(LineInfo, line)));
    traceback_cls->freeze();
}
}
