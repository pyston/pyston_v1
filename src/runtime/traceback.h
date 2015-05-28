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
    typedef llvm::SmallVector<LineInfo, 1> LinesVector;
    Box* tb_next;
    LinesVector lines;
    Box* py_lines;

    BoxedTraceback(LinesVector&& lines) : tb_next(None), lines(std::move(lines)), py_lines(NULL) {}
    BoxedTraceback(BoxedTraceback* tb_next) : tb_next(tb_next), py_lines(NULL) {}
    BoxedTraceback() : tb_next(None), py_lines(NULL) {}

    DEFAULT_CLASS(traceback_cls);

    void addLine(const LineInfo line);

    static Box* getLines(Box* b);

    static void gcHandler(gc::GCVisitor* v, Box* b);

    // somewhat equivalent to PyTraceBack_Here
    static void Here(LineInfo lineInfo, BoxedTraceback** tb);
};

void printTraceback(Box* b);
void setupTraceback();
}

#endif
