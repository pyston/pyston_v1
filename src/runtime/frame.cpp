// Copyright (c) 2014-2016 Dropbox, Inc.
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

#include "frameobject.h"
#include "pythread.h"

#include "codegen/unwinding.h"
#include "core/ast.h"
#include "runtime/types.h"

namespace pyston {

extern "C" {
BoxedClass* frame_cls;
}

// Issues:
// - breaks gdb backtraces
// - breaks c++ exceptions
// - we never free the trampolines
class BoxedFrame : public Box {
private:
    // Call boxFrame to get a BoxedFrame object.
    BoxedFrame(FrameInfo* frame_info) __attribute__((visibility("default")))
    : frame_info(frame_info), _back(NULL), _code(NULL), _globals(NULL), _locals(NULL), _linenumber(-1) {}

public:
    FrameInfo* frame_info;

    Box* _back;
    Box* _code;
    Box* _globals;
    Box* _locals;

    int _linenumber;


    bool hasExited() const { return frame_info == NULL; }


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
        Box::gcHandler(v, b);

        auto f = static_cast<BoxedFrame*>(b);

        v->visit(&f->_back);
        v->visit(&f->_code);
        v->visit(&f->_globals);
        v->visit(&f->_locals);
    }

    static Box* code(Box* obj, void*) {
        auto f = static_cast<BoxedFrame*>(obj);

        if (!f->_code)
            f->_code = (Box*)f->frame_info->md->getCode();
        return f->_code;
    }

    static Box* locals(Box* obj, void*) {
        auto f = static_cast<BoxedFrame*>(obj);

        if (f->hasExited())
            return f->_locals;
        return f->frame_info->updateBoxedLocals();
    }

    static Box* globals(Box* obj, void*) {
        auto f = static_cast<BoxedFrame*>(obj);

        if (!f->_globals) {
            f->_globals = f->frame_info->globals;
            if (f->_globals && PyModule_Check(f->_globals))
                f->_globals = f->_globals->getAttrWrapper();
        }
        return f->_globals;
    }

    static Box* back(Box* obj, void*) {
        auto f = static_cast<BoxedFrame*>(obj);

        if (!f->_back) {
            if (!f->frame_info->back)
                f->_back = None;
            else
                f->_back = BoxedFrame::boxFrame(f->frame_info->back);
        }
        return f->_back;
    }

    static Box* lineno(Box* obj, void*) {
        auto f = static_cast<BoxedFrame*>(obj);

        if (f->hasExited())
            return boxInt(f->_linenumber);

        AST_stmt* stmt = f->frame_info->stmt;
        return boxInt(stmt->lineno);
    }

    void handleFrameExit() {
        if (hasExited())
            return;

        _back = back(this, NULL);
        _code = code(this, NULL);
        _globals = globals(this, NULL);
        _locals = locals(this, NULL);
        _linenumber = frame_info->stmt->lineno;

        frame_info = NULL; // this means exited == true
        assert(hasExited());
    }

    DEFAULT_CLASS(frame_cls);

    static Box* boxFrame(FrameInfo* fi) {
        if (fi->frame_obj == NULL)
            fi->frame_obj = new BoxedFrame(fi);
        assert(fi->frame_obj->cls == frame_cls);
        return fi->frame_obj;
    }

    static Box* boxFrame(Box* back, BoxedCode* code, Box* globals, Box* locals) {
        BoxedFrame* frame = new BoxedFrame(NULL);
        frame->_back = back;
        frame->_code = (Box*)code;
        frame->_globals = globals;
        frame->_locals = locals;
        frame->_linenumber = -1;
        return frame;
    }
};


Box* getFrame(FrameInfo* frame_info) {
    return BoxedFrame::boxFrame(frame_info);
}

Box* getFrame(int depth) {
    FrameInfo* frame_info = getPythonFrameInfo(depth);
    if (!frame_info)
        return NULL;
    return BoxedFrame::boxFrame(frame_info);
}

void frameInvalidateBack(BoxedFrame* frame) {
    RELEASE_ASSERT(!frame->hasExited(), "should not happen");
    frame->_back = NULL;
}

extern "C" void initFrame(FrameInfo* frame_info) {
    frame_info->back = (FrameInfo*)(cur_thread_state.frame_info);
    cur_thread_state.frame_info = frame_info;
}

extern "C" void deinitFrame(FrameInfo* frame_info) {
    cur_thread_state.frame_info = frame_info->back;
    BoxedFrame* frame = frame_info->frame_obj;
    if (frame)
        frame->handleFrameExit();
}

extern "C" int PyFrame_GetLineNumber(PyFrameObject* _f) noexcept {
    BoxedInt* lineno = (BoxedInt*)BoxedFrame::lineno((Box*)_f, NULL);
    return lineno->n;
}

extern "C" void PyFrame_SetLineNumber(PyFrameObject* _f, int linenumber) noexcept {
    BoxedFrame* f = (BoxedFrame*)_f;
    RELEASE_ASSERT(f->hasExited(),
                   "if this frame did not exit yet the line number may get overwriten, may be a problem?");
    f->_linenumber = linenumber;
}

extern "C" PyFrameObject* PyFrame_New(PyThreadState* tstate, PyCodeObject* code, PyObject* globals,
                                      PyObject* locals) noexcept {
    RELEASE_ASSERT(tstate == &cur_thread_state, "");

    RELEASE_ASSERT(PyCode_Check((Box*)code), "");
    RELEASE_ASSERT(!globals || PyDict_Check(globals) || globals->cls == attrwrapper_cls, "%s", globals->cls->tp_name);
    RELEASE_ASSERT(!locals || PyDict_Check(locals), "%s", locals->cls->tp_name);
    return (PyFrameObject*)BoxedFrame::boxFrame(getFrame(0), (BoxedCode*)code, globals, locals);
}

extern "C" PyObject* PyFrame_GetGlobals(PyFrameObject* f) noexcept {
    return BoxedFrame::globals((Box*)f, NULL);
}
extern "C" PyObject* PyFrame_GetCode(PyFrameObject* f) noexcept {
    return BoxedFrame::code((Box*)f, NULL);
}

extern "C" PyFrameObject* PyFrame_ForStackLevel(int stack_level) noexcept {
    return (PyFrameObject*)getFrame(stack_level);
}

void setupFrame() {
    frame_cls = BoxedClass::create(type_cls, object_cls, &BoxedFrame::gchandler, 0, 0, sizeof(BoxedFrame), false,
                                   "frame", false);
    frame_cls->has_safe_tp_dealloc = true;

    frame_cls->giveAttrDescriptor("f_code", BoxedFrame::code, NULL);
    frame_cls->giveAttrDescriptor("f_locals", BoxedFrame::locals, NULL);
    frame_cls->giveAttrDescriptor("f_lineno", BoxedFrame::lineno, NULL);

    frame_cls->giveAttrDescriptor("f_globals", BoxedFrame::globals, NULL);
    frame_cls->giveAttrDescriptor("f_back", BoxedFrame::back, NULL);

    frame_cls->freeze();
}
}
