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
#include "gc/collector.h"
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

    if (self->py_lines)
        v->visit(self->py_lines);
    if (self->tb_next)
        v->visit(self->tb_next);

    boxGCHandler(v, b);
}

void printTraceback(Box* b) {
    if (b == None)
        return;
    assert(b->cls == traceback_cls);

    BoxedTraceback* tb = static_cast<BoxedTraceback*>(b);

    fprintf(stderr, "Traceback (most recent call last):\n");

    for (; tb && tb != None; tb = static_cast<BoxedTraceback*>(tb->tb_next)) {
        auto& line = tb->line;
        fprintf(stderr, "  File \"%s\", line %d, in %s:\n", line.file.c_str(), line.line, line.func.c_str());

        if (line.line < 0)
            continue;

        FILE* f = fopen(line.file.c_str(), "r");
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

Box* BoxedTraceback::getLines(Box* b) {
    assert(b->cls == traceback_cls);

    BoxedTraceback* tb = static_cast<BoxedTraceback*>(b);

    if (!tb->py_lines) {
        BoxedList* lines = new BoxedList();
        for (BoxedTraceback* wtb = tb; wtb && wtb != None; wtb = static_cast<BoxedTraceback*>(wtb->tb_next)) {
            auto& line = wtb->line;
            auto l = BoxedTuple::create({ boxString(line.file), boxString(line.func), boxInt(line.line) });
            listAppendInternal(lines, l);
        }
        tb->py_lines = lines;
    }

    return tb->py_lines;
}

void BoxedTraceback::here(LineInfo lineInfo, Box** tb) {
    *tb = new BoxedTraceback(std::move(lineInfo), *tb);
}

void setupTraceback() {
    traceback_cls = BoxedHeapClass::create(type_cls, object_cls, BoxedTraceback::gcHandler, 0, 0,
                                           sizeof(BoxedTraceback), false, "traceback");

    traceback_cls->giveAttr("getLines", new BoxedFunction(boxRTFunction((void*)BoxedTraceback::getLines, UNKNOWN, 1)));

    traceback_cls->freeze();
}
}
