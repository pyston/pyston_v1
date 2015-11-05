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

#ifndef PYSTON_RUNTIME_TRACEBACK_H
#define PYSTON_RUNTIME_TRACEBACK_H

#include "core/types.h"
#include "runtime/types.h"

namespace pyston {

namespace gc {
class GCVisitor;
}

extern "C" BoxedClass* traceback_cls;
class BoxedTraceback : public Box {
public:
    LineInfo line;
    Box* tb_next;
    Box* tb_frame;

    BoxedTraceback(LineInfo line, Box* tb_next, Box* tb_frame)
        : line(std::move(line)), tb_next(tb_next), tb_frame(tb_frame) {
        if (!tb_frame)
            this->tb_frame = None;
    }

    DEFAULT_CLASS(traceback_cls);

    static Box* lineno(Box* obj, void*);

    static void gcHandler(gc::GCVisitor* v, Box* b);

    // somewhat equivalent to PyTraceBack_Here
    static void here(LineInfo lineInfo, Box** tb, Box* frame);
};

void printTraceback(Box* b);
void setupTraceback();
}

#endif
