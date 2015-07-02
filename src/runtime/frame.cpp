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

#include "Python.h"
#include "pythread.h"

#include "codegen/unwinding.h"
#include "core/ast.h"
#include "runtime/types.h"

namespace pyston {

BoxedClass* frame_cls;

// Issues:
// - breaks gdb backtraces
// - breaks c++ exceptions
// - we never free the trampolines
class BoxedFrame : public Box {
private:
    // Call boxFrame to get a BoxedFrame object.
    BoxedFrame(PythonFrameIterator it) __attribute__((visibility("default")))
    : it(std::move(it)), thread_id(PyThread_get_thread_ident()) {}

public:
    PythonFrameIterator it;
    long thread_id;

    Box* _globals;
    Box* _code;

    void update() {
        // This makes sense as an exception, but who knows how the user program would react
        // (it might swallow it and do something different)
        RELEASE_ASSERT(thread_id == PyThread_get_thread_ident(),
                       "frame objects can only be accessed from the same thread");
        PythonFrameIterator new_it = it.getCurrentVersion();
        RELEASE_ASSERT(new_it.exists() && new_it.getFrameInfo()->frame_obj == this, "frame has exited");
        it = std::move(new_it);
    }

    // cpython frame objects have the following attributes

    // read-only attributes
    //
    // f_back[*]       : previous stack frame (toward caller)
    // f_code          : code object being executed in this frame
    // f_locals        : dictionary used to look up local variables in this frame
    // f_globals       : dictionary used to look up global variables in this frame
    // f_builtins[*]   : dictionary to look up built-in (intrinsic) names
    // f_restricted[*] : whether this function is executing in restricted execution mode
    // f_lasti[*]      : precise instruction (this is an index into the bytecode string of the code object)

    // writable attributes
    //
    // f_trace[*]         : if not None, is a function called at the start of each source code line (used by debugger)
    // f_exc_type[*],     : represent the last exception raised in the parent frame provided another exception was
    // f_exc_value[*],    : ever raised in the current frame (in all other cases they are None).
    // f_exc_traceback[*] :
    // f_lineno[**]       : the current line number of the frame -- writing to this from within a trace function jumps
    // to
    //                    : the given line (only for the bottom-most frame).  A debugger can implement a Jump command
    //                    (aka
    //                    : Set Next Statement) by writing to f_lineno
    //
    // * = unsupported in Pyston
    // ** = getter supported, but setter unsupported

    static void gchandler(GCVisitor* v, Box* b) {
        boxGCHandler(v, b);

        auto f = static_cast<BoxedFrame*>(b);

        v->visit(f->_code);
        v->visit(f->_globals);
    }

    static void simpleDestructor(Box* b) {
        auto f = static_cast<BoxedFrame*>(b);

        f->it.~PythonFrameIterator();
    }

    static Box* code(Box* obj, void*) {
        auto f = static_cast<BoxedFrame*>(obj);
        return f->_code;
    }

    static Box* locals(Box* obj, void*) {
        auto f = static_cast<BoxedFrame*>(obj);
        f->update();
        return f->it.fastLocalsToBoxedLocals();
    }

    static Box* globals(Box* obj, void*) {
        auto f = static_cast<BoxedFrame*>(obj);
        return f->_globals;
    }

    static Box* back(Box* obj, void*) {
        auto f = static_cast<BoxedFrame*>(obj);
        f->update();

        PythonFrameIterator it = f->it.back();
        if (!it.exists())
            return None;
        return BoxedFrame::boxFrame(std::move(it));
    }

    static Box* lineno(Box* obj, void*) {
        auto f = static_cast<BoxedFrame*>(obj);
        f->update();
        AST_stmt* stmt = f->it.getCurrentStatement();
        return boxInt(stmt->lineno);
    }

    DEFAULT_CLASS(frame_cls);

    static Box* boxFrame(PythonFrameIterator it) {
        FrameInfo* fi = it.getFrameInfo();
        if (fi->frame_obj == NULL) {
            auto cl = it.getCL();
            Box* globals = it.getGlobalsDict();
            BoxedFrame* f = fi->frame_obj = new BoxedFrame(std::move(it));
            f->_globals = globals;
            f->_code = codeForCLFunction(cl);
        }

        return fi->frame_obj;
    }
};

Box* getFrame(int depth) {
    auto it = getPythonFrame(depth);
    if (!it.exists())
        return NULL;

    return BoxedFrame::boxFrame(std::move(it));
}

void setupFrame() {
    frame_cls = BoxedHeapClass::create(type_cls, object_cls, &BoxedFrame::gchandler, 0, 0, sizeof(BoxedFrame), false,
                                       "frame");
    frame_cls->tp_dealloc = BoxedFrame::simpleDestructor;
    frame_cls->has_safe_tp_dealloc = true;

    frame_cls->giveAttr("f_code", new (pyston_getset_cls) BoxedGetsetDescriptor(BoxedFrame::code, NULL, NULL));
    frame_cls->giveAttr("f_locals", new (pyston_getset_cls) BoxedGetsetDescriptor(BoxedFrame::locals, NULL, NULL));
    frame_cls->giveAttr("f_lineno", new (pyston_getset_cls) BoxedGetsetDescriptor(BoxedFrame::lineno, NULL, NULL));

    frame_cls->giveAttr("f_globals", new (pyston_getset_cls) BoxedGetsetDescriptor(BoxedFrame::globals, NULL, NULL));
    frame_cls->giveAttr("f_back", new (pyston_getset_cls) BoxedGetsetDescriptor(BoxedFrame::back, NULL, NULL));

    frame_cls->freeze();
}
}
