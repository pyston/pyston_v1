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

void BoxedTraceback::gcHandler(GCVisitor* v, Box* b) {
    assert(b->cls == traceback_cls);
    BoxedTraceback* self = static_cast<BoxedTraceback*>(b);

    v->visit(&self->tb_next);
    v->visit(&self->tb_frame);
    v->visit(&self->line.file);
    v->visit(&self->line.func);

    Box::gcHandler(v, b);
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

Box* BoxedTraceback::lineno(Box* obj, void*) {
    auto f = static_cast<BoxedTraceback*>(obj);
    assert(f->cls == traceback_cls);

    return boxInt(f->line.line);
}

void BoxedTraceback::here(LineInfo lineInfo, Box** tb, Box* frame) {
    *tb = new BoxedTraceback(std::move(lineInfo), *tb, frame);
}

static Box* traceback_tb_next(Box* self, void*) {
    assert(self->cls == traceback_cls);

    BoxedTraceback* traceback = static_cast<BoxedTraceback*>(self);
    return traceback->tb_next;
}

void setupTraceback() {
    traceback_cls = BoxedClass::create(type_cls, object_cls, BoxedTraceback::gcHandler, 0, 0, sizeof(BoxedTraceback),
                                       false, "traceback");

    traceback_cls->giveAttr(
        "tb_frame", new BoxedMemberDescriptor(BoxedMemberDescriptor::OBJECT, offsetof(BoxedTraceback, tb_frame)));
    traceback_cls->giveAttr(
        "tb_next", new BoxedMemberDescriptor(BoxedMemberDescriptor::OBJECT, offsetof(BoxedTraceback, tb_next)));
    traceback_cls->giveAttr("tb_lineno",
                            new (pyston_getset_cls) BoxedGetsetDescriptor(BoxedTraceback::lineno, NULL, NULL));

    /*
     * Currently not supported.
    traceback_cls->giveAttr("tb_frame", new (pyston_getset_cls) BoxedGetsetDescriptor(traceback_tb_frame, NULL, NULL));
    traceback_cls->giveAttr("tb_lasti", new (pyston_getset_cls) BoxedGetsetDescriptor(traceback_tb_lasti, NULL, NULL));
    traceback_cls->giveAttr("tb_lineno", new (pyston_getset_cls) BoxedGetsetDescriptor(traceback_tb_lineno, NULL,
    NULL));
    */
    traceback_cls->giveAttr("tb_next", new (pyston_getset_cls) BoxedGetsetDescriptor(traceback_tb_next, NULL, NULL));

    traceback_cls->freeze();
}
}
