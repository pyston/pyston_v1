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
//

#include <sstream>

#include "codegen/baseline_jit.h"
#include "core/ast.h"
#include "runtime/objmodel.h"
#include "runtime/set.h"

namespace pyston {

extern "C" {
BoxedClass* code_cls;
}

BORROWED(Box*) BoxedCode::name(Box* b, void*) noexcept {
    RELEASE_ASSERT(b->cls == code_cls, "");
    BoxedCode* code = static_cast<BoxedCode*>(b);
    if (code->_name)
        return code->_name;
    return code->source->getName();
}

Box* BoxedCode::co_name(Box* b, void* arg) noexcept {
    return incref(name(b, arg));
}

BORROWED(Box*) BoxedCode::filename(Box* b, void*) noexcept {
    RELEASE_ASSERT(b->cls == code_cls, "");
    BoxedCode* code = static_cast<BoxedCode*>(b);
    if (code->_filename)
        return code->_filename;
    return code->source->getFn();
}

Box* BoxedCode::co_filename(Box* b, void* arg) noexcept {
    return incref(filename(b, arg));
}

Box* BoxedCode::firstlineno(Box* b, void*) noexcept {
    RELEASE_ASSERT(b->cls == code_cls, "");
    BoxedCode* code = static_cast<BoxedCode*>(b);

    if (!code || !code->source)
        return boxInt(code->_firstline);

    if (code->source->ast->lineno == (uint32_t)-1)
        return boxInt(-1);

    return boxInt(code->source->ast->lineno);
}

Box* BoxedCode::argcount(Box* b, void*) noexcept {
    RELEASE_ASSERT(b->cls == code_cls, "");
    return boxInt(static_cast<BoxedCode*>(b)->num_args);
}

Box* BoxedCode::varnames(Box* b, void*) noexcept {
    RELEASE_ASSERT(b->cls == code_cls, "");
    BoxedCode* code = static_cast<BoxedCode*>(b);

    auto& param_names = code->param_names;
    if (!param_names.takes_param_names)
        return incref(EmptyTuple);

    std::vector<Box*> elts;
    for (auto sr : param_names.allArgsAsStr())
        elts.push_back(boxString(sr));
    auto rtn = BoxedTuple::create(elts.size(), &elts[0]);
    for (auto e : elts)
        Py_DECREF(e);
    return rtn;
}

Box* BoxedCode::flags(Box* b, void*) noexcept {
    RELEASE_ASSERT(b->cls == code_cls, "");
    BoxedCode* code = static_cast<BoxedCode*>(b);

    int flags = 0;
    if (code->param_names.has_vararg_name)
        flags |= CO_VARARGS;
    if (code->param_names.has_kwarg_name)
        flags |= CO_VARKEYWORDS;
    if (code->isGenerator())
        flags |= CO_GENERATOR;
    return boxInt(flags);
}

void BoxedCode::dealloc(Box* b) noexcept {
    BoxedCode* o = static_cast<BoxedCode*>(b);

    Py_XDECREF(o->_filename);
    Py_XDECREF(o->_name);

    o->cls->tp_free(o);
}


// The dummy constructor for PyCode_New:
BoxedCode::BoxedCode(Box* filename, Box* name, int firstline)
    : _filename(filename),
      _name(name),
      _firstline(firstline),
      param_names(ParamNames::empty()),
      takes_varargs(false),
      takes_kwargs(false),
      num_args(0),
      internal_callable(nullptr, nullptr) {
    Py_XINCREF(filename);
    Py_XINCREF(name);
}

extern "C" PyCodeObject* PyCode_New(int argcount, int nlocals, int stacksize, int flags, PyObject* code,
                                    PyObject* consts, PyObject* names, PyObject* varnames, PyObject* freevars,
                                    PyObject* cellvars, PyObject* filename, PyObject* name, int firstlineno,
                                    PyObject* lnotab) noexcept {
    // Check if this is a dummy code object like PyCode_NewEmpty generates.
    // Because we currently support dummy ones only.
    bool is_dummy = argcount == 0 && nlocals == 0 && stacksize == 0 && flags == 0;
    is_dummy = is_dummy && code == EmptyString && lnotab == EmptyString;
    for (auto&& var : { consts, names, varnames, freevars, cellvars })
        is_dummy = is_dummy && var == EmptyTuple;
    // The follwing variables are not implemented but we allow them because there is currently
    // no way for code to retrieve them.
    auto temp_allowed = argcount || argcount || flags || varnames != EmptyTuple;
    RELEASE_ASSERT(is_dummy || temp_allowed, "not implemented");

    RELEASE_ASSERT(PyString_Check(filename), "");
    RELEASE_ASSERT(PyString_Check(name), "");

    return (PyCodeObject*)new BoxedCode(filename, name, firstlineno);
}

extern "C" PyCodeObject* PyCode_NewEmpty(const char* filename, const char* funcname, int firstlineno) noexcept {
    static PyObject* emptystring = NULL;
    static PyObject* nulltuple = NULL;
    PyObject* filename_ob = NULL;
    PyObject* funcname_ob = NULL;
    PyCodeObject* result = NULL;
    if (emptystring == NULL) {
        emptystring = PyGC_RegisterStaticConstant(PyString_FromString(""));
        if (emptystring == NULL)
            goto failed;
    }
    if (nulltuple == NULL) {
        nulltuple = PyGC_RegisterStaticConstant(PyTuple_New(0));
        if (nulltuple == NULL)
            goto failed;
    }
    funcname_ob = PyString_FromString(funcname);
    if (funcname_ob == NULL)
        goto failed;
    filename_ob = PyString_FromString(filename);
    if (filename_ob == NULL)
        goto failed;

    result = PyCode_New(0,           /* argcount */
                        0,           /* nlocals */
                        0,           /* stacksize */
                        0,           /* flags */
                        emptystring, /* code */
                        nulltuple,   /* consts */
                        nulltuple,   /* names */
                        nulltuple,   /* varnames */
                        nulltuple,   /* freevars */
                        nulltuple,   /* cellvars */
                        filename_ob, /* filename */
                        funcname_ob, /* name */
                        firstlineno, /* firstlineno */
                        emptystring  /* lnotab */
                        );

failed:
    Py_XDECREF(funcname_ob);
    Py_XDECREF(filename_ob);
    return result;
}

extern "C" int PyCode_GetArgCount(PyCodeObject* op) noexcept {
    RELEASE_ASSERT(PyCode_Check((Box*)op), "");
    return unboxInt(autoDecref(BoxedCode::argcount((Box*)op, NULL)));
}

extern "C" BORROWED(PyObject*) PyCode_GetFilename(PyCodeObject* op) noexcept {
    RELEASE_ASSERT(PyCode_Check((Box*)op), "");
    return BoxedCode::filename((Box*)op, NULL);
}

extern "C" BORROWED(PyObject*) PyCode_GetName(PyCodeObject* op) noexcept {
    RELEASE_ASSERT(PyCode_Check((Box*)op), "");
    return BoxedCode::name((Box*)op, NULL);
}

extern "C" int PyCode_HasFreeVars(PyCodeObject* _code) noexcept {
    BoxedCode* code = (BoxedCode*)_code;
    return code->source->scoping.takesClosure() ? 1 : 0;
}

void setupCode() {
    code_cls->giveAttrBorrowed("__new__", Py_None); // Hacky way of preventing users from instantiating this

    code_cls->giveAttrDescriptor("co_name", BoxedCode::co_name, NULL);
    code_cls->giveAttrDescriptor("co_filename", BoxedCode::co_filename, NULL);
    code_cls->giveAttrDescriptor("co_firstlineno", BoxedCode::firstlineno, NULL);
    code_cls->giveAttrDescriptor("co_argcount", BoxedCode::argcount, NULL);
    code_cls->giveAttrDescriptor("co_varnames", BoxedCode::varnames, NULL);
    code_cls->giveAttrDescriptor("co_flags", BoxedCode::flags, NULL);

    code_cls->freeze();
}
}
