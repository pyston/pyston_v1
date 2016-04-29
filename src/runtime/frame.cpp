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
#include "core/cfg.h"
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

    static BORROWED(Box*) code(Box* obj, void*) {
        auto f = static_cast<BoxedFrame*>(obj);

        if (!f->_code)
            f->_code = incref((Box*)f->frame_info->md->getCode());

        return f->_code;
    }

    static Box* f_code(Box* obj, void* arg) { return incref(code(obj, arg)); }

    static BORROWED(Box*) locals(Box* obj, void*) {
        auto f = static_cast<BoxedFrame*>(obj);

        if (f->hasExited())
            return f->_locals;

        return f->frame_info->updateBoxedLocals();
    }

    static Box* f_locals(Box* obj, void* arg) { return incref(locals(obj, arg)); }

    static BORROWED(Box*) globals(Box* obj, void*) {
        auto f = static_cast<BoxedFrame*>(obj);

        if (!f->_globals) {
            Box* globals = f->frame_info->globals;
            if (globals && PyModule_Check(globals))
                f->_globals = incref(globals->getAttrWrapper());
            else {
                f->_globals = incref(globals);
            }
        }

        return f->_globals;
    }

    static Box* f_globals(Box* obj, void* arg) { return incref(globals(obj, arg)); }

    static BORROWED(Box*) back(Box* obj, void*) {
        auto f = static_cast<BoxedFrame*>(obj);

        if (!f->_back) {
            if (!f->frame_info->back)
                f->_back = incref(None);
            else
                f->_back = incref(BoxedFrame::boxFrame(f->frame_info->back));
        }

        return f->_back;
    }

    static Box* f_back(Box* obj, void* arg) { return incref(back(obj, arg)); }

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

        // Call the getters for their side-effects of caching the result:
        back(this, NULL);
        code(this, NULL);
        globals(this, NULL);
        assert(!_locals);
        _locals = incref(locals(this, NULL));
        _linenumber = frame_info->stmt->lineno;

        frame_info = NULL; // this means exited == true
        assert(hasExited());
    }

    DEFAULT_CLASS(frame_cls);

    static BORROWED(Box*) boxFrame(FrameInfo* fi) {
        if (fi->frame_obj == NULL)
            fi->frame_obj = new BoxedFrame(fi);
        assert(fi->frame_obj->cls == frame_cls);
        return fi->frame_obj;
    }

    static void dealloc(Box* b) noexcept {
        BoxedFrame* f = static_cast<BoxedFrame*>(b);

        _PyObject_GC_UNTRACK(f);
        clear(b);
        f->cls->tp_free(b);
    }
    static int traverse(Box* self, visitproc visit, void* arg) noexcept {
        BoxedFrame* o = static_cast<BoxedFrame*>(self);
        Py_VISIT(o->_back);
        Py_VISIT(o->_code);
        Py_VISIT(o->_globals);
        Py_VISIT(o->_locals);
        return 0;
    }
    static int clear(Box* self) noexcept {
        BoxedFrame* o = static_cast<BoxedFrame*>(self);
        assert(o->hasExited());
        Py_CLEAR(o->_back);
        Py_CLEAR(o->_code);
        Py_CLEAR(o->_globals);
        Py_CLEAR(o->_locals);
        return 0;
    }

    static Box* createFrame(Box* back, BoxedCode* code, Box* globals, Box* locals) {
        BoxedFrame* frame = new BoxedFrame(NULL);
        frame->_back = xincref(back);
        frame->_code = (Box*)xincref(code);
        frame->_globals = xincref(globals);
        frame->_locals = xincref(locals);
        frame->_linenumber = -1;
        return frame;
    }
};

extern "C" int PyFrame_ClearFreeList(void) noexcept {
    return 0; // number of entries cleared
}

BORROWED(Box*) getFrame(FrameInfo* frame_info) {
    return BoxedFrame::boxFrame(frame_info);
}

BORROWED(Box*) getFrame(int depth) {
    FrameInfo* frame_info = getPythonFrameInfo(depth);
    if (!frame_info)
        return NULL;
    return BoxedFrame::boxFrame(frame_info);
}

void frameInvalidateBack(BoxedFrame* frame) {
    RELEASE_ASSERT(!frame->hasExited(), "should not happen");
    Py_CLEAR(frame->_back);
}

extern "C" void initFrame(FrameInfo* frame_info) {
    frame_info->back = (FrameInfo*)(cur_thread_state.frame_info);
    cur_thread_state.frame_info = frame_info;
}

FrameInfo* const FrameInfo::NO_DEINIT = (FrameInfo*)-2; // not -1 to not match memset(-1)

void FrameInfo::disableDeinit(FrameInfo* replacement_frame) {
    assert(replacement_frame->back == this->back);
    assert(replacement_frame->frame_obj == this->frame_obj);

    if (this->frame_obj) {
        assert(this->frame_obj->frame_info == this);
        this->frame_obj->frame_info = replacement_frame;
    }

#ifndef NDEBUG
    // First, make sure this doesn't get used for anything else:
    memset(this, -1, sizeof(*this));
#endif

    // Kinda hacky but maybe worth it to not store any extra bits:
    back = NO_DEINIT;
}

extern "C" void deinitFrameMaybe(FrameInfo* frame_info) {
    // Note: this has to match FrameInfo::disableDeinit
    if (frame_info->back != FrameInfo::NO_DEINIT)
        deinitFrame(frame_info);
}

extern "C" void deinitFrame(FrameInfo* frame_info) {
    // This can fire if we have a call to deinitFrame() that should be to deinitFrameMaybe() instead
    assert(frame_info->back != FrameInfo::NO_DEINIT);

    assert(cur_thread_state.frame_info == frame_info);
    cur_thread_state.frame_info = frame_info->back;
    BoxedFrame* frame = frame_info->frame_obj;
    if (frame) {
        frame->handleFrameExit();
        Py_CLEAR(frame_info->frame_obj);
    }

    assert(frame_info->vregs || frame_info->num_vregs == 0);
    int num_vregs = frame_info->num_vregs;
    assert(num_vregs >= 0);

    decrefArray<true>(frame_info->vregs, num_vregs);

    Py_CLEAR(frame_info->boxedLocals);

    if (frame_info->exc.type) {
        Py_CLEAR(frame_info->exc.type);
        Py_CLEAR(frame_info->exc.value);
        Py_CLEAR(frame_info->exc.traceback);
    }

    Py_CLEAR(frame_info->globals);
}

int frameinfo_traverse(FrameInfo* frame_info, visitproc visit, void* arg) noexcept {
    Py_VISIT(frame_info->frame_obj);

    if (frame_info->vregs) {
        int num_vregs = frame_info->num_vregs;
        for (int i = 0; i < num_vregs; i++) {
            Py_VISIT(frame_info->vregs[i]);
        }
    }
    Py_VISIT(frame_info->boxedLocals);

    if (frame_info->exc.type) {
        Py_VISIT(frame_info->exc.type);
        Py_VISIT(frame_info->exc.value);
        Py_VISIT(frame_info->exc.traceback);
    }

    return 0;
}

extern "C" void setFrameExcInfo(FrameInfo* frame_info, STOLEN(Box*) type, STOLEN(Box*) value, STOLEN(Box*) tb) {
    Box* old_type = frame_info->exc.type;
    Box* old_value = frame_info->exc.value;
    Box* old_traceback = frame_info->exc.traceback;

    frame_info->exc.type = type;
    frame_info->exc.value = value;
    frame_info->exc.traceback = tb;

    if (old_type) {
        Py_DECREF(old_type);
        Py_DECREF(old_value);
        Py_XDECREF(old_traceback);
    }
}

extern "C" int PyFrame_GetLineNumber(PyFrameObject* _f) noexcept {
    BoxedInt* lineno = (BoxedInt*)BoxedFrame::lineno((Box*)_f, NULL);
    return autoDecref(lineno)->n;
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
    return (PyFrameObject*)BoxedFrame::createFrame(getFrame(0), (BoxedCode*)code, globals, locals);
}

extern "C" BORROWED(PyObject*) PyFrame_GetGlobals(PyFrameObject* f) noexcept {
    return BoxedFrame::globals((Box*)f, NULL);
}
extern "C" BORROWED(PyObject*) PyFrame_GetCode(PyFrameObject* f) noexcept {
    return BoxedFrame::code((Box*)f, NULL);
}

extern "C" PyFrameObject* PyFrame_ForStackLevel(int stack_level) noexcept {
    return (PyFrameObject*)getFrame(stack_level);
}

void setupFrame() {
    frame_cls = BoxedClass::create(type_cls, object_cls, 0, 0, sizeof(BoxedFrame), false, "frame", false,
                                   (destructor)BoxedFrame::dealloc, NULL, true, (traverseproc)BoxedFrame::traverse,
                                   (inquiry)BoxedFrame::clear);

    frame_cls->giveAttrDescriptor("f_code", BoxedFrame::f_code, NULL);
    frame_cls->giveAttrDescriptor("f_locals", BoxedFrame::f_locals, NULL);
    frame_cls->giveAttrDescriptor("f_lineno", BoxedFrame::lineno, NULL);

    frame_cls->giveAttrDescriptor("f_globals", BoxedFrame::f_globals, NULL);
    frame_cls->giveAttrDescriptor("f_back", BoxedFrame::f_back, NULL);

    frame_cls->freeze();
}
}
