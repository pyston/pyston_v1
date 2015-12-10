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
//
#include "runtime/code.h"

#include <sstream>

#include "core/ast.h"
#include "runtime/objmodel.h"
#include "runtime/set.h"

namespace pyston {

extern "C" {
BoxedClass* code_cls;
}

void BoxedCode::gcHandler(GCVisitor* v, Box* b) {
    assert(b->cls == code_cls);
    Box::gcHandler(v, b);
}

Box* BoxedCode::name(Box* b, void*) {
    RELEASE_ASSERT(b->cls == code_cls, "");
    return static_cast<BoxedCode*>(b)->f->source->getName();
}

Box* BoxedCode::filename(Box* b, void*) {
    RELEASE_ASSERT(b->cls == code_cls, "");
    return static_cast<BoxedCode*>(b)->f->source->getFn();
}

Box* BoxedCode::firstlineno(Box* b, void*) {
    RELEASE_ASSERT(b->cls == code_cls, "");
    BoxedCode* code = static_cast<BoxedCode*>(b);
    FunctionMetadata* md = code->f;

    if (!md->source) {
        // I don't think it really matters what we return here;
        // in CPython, builtin functions don't have code objects.
        return boxInt(-1);
    }

    if (md->source->ast->lineno == (uint32_t)-1)
        return boxInt(-1);

    return boxInt(md->source->ast->lineno);
}

Box* BoxedCode::argcount(Box* b, void*) {
    RELEASE_ASSERT(b->cls == code_cls, "");

    return boxInt(static_cast<BoxedCode*>(b)->f->num_args);
}

Box* BoxedCode::varnames(Box* b, void*) {
    RELEASE_ASSERT(b->cls == code_cls, "");
    BoxedCode* code = static_cast<BoxedCode*>(b);

    auto& param_names = code->f->param_names;
    if (!param_names.takes_param_names)
        return EmptyTuple;

    std::vector<Box*, StlCompatAllocator<Box*>> elts;
    for (auto sr : param_names.args)
        elts.push_back(boxString(sr));
    if (param_names.vararg.size())
        elts.push_back(boxString(param_names.vararg));
    if (param_names.kwarg.size())
        elts.push_back(boxString(param_names.kwarg));
    return BoxedTuple::create(elts.size(), &elts[0]);
}

Box* BoxedCode::flags(Box* b, void*) {
    RELEASE_ASSERT(b->cls == code_cls, "");
    BoxedCode* code = static_cast<BoxedCode*>(b);

    int flags = 0;
    if (code->f->param_names.vararg.size())
        flags |= CO_VARARGS;
    if (code->f->param_names.kwarg.size())
        flags |= CO_VARKEYWORDS;
    if (code->f->isGenerator())
        flags |= CO_GENERATOR;
    return boxInt(flags);
}

Box* codeForFunction(BoxedFunction* f) {
    return f->md->getCode();
}

FunctionMetadata* metadataFromCode(Box* code) {
    assert(code->cls == code_cls);
    return static_cast<BoxedCode*>(code)->f;
}

extern "C" PyCodeObject* PyCode_New(int, int, int, int, PyObject*, PyObject*, PyObject*, PyObject*, PyObject*,
                                    PyObject*, PyObject*, PyObject*, int, PyObject*) noexcept {
    RELEASE_ASSERT(0, "not implemented");
}

extern "C" int PyCode_GetArgCount(PyCodeObject* op) noexcept {
    RELEASE_ASSERT(PyCode_Check((Box*)op), "");
    return unboxInt(BoxedCode::argcount((Box*)op, NULL));
}

void setupCode() {
    code_cls = BoxedClass::create(type_cls, object_cls, &BoxedCode::gcHandler, 0, 0, sizeof(BoxedCode), false, "code");

    code_cls->giveAttr("__new__", None); // Hacky way of preventing users from instantiating this

    code_cls->giveAttrDescriptor("co_name", BoxedCode::name, NULL);
    code_cls->giveAttrDescriptor("co_filename", BoxedCode::filename, NULL);
    code_cls->giveAttrDescriptor("co_firstlineno", BoxedCode::firstlineno, NULL);
    code_cls->giveAttrDescriptor("co_argcount", BoxedCode::argcount, NULL);
    code_cls->giveAttrDescriptor("co_varnames", BoxedCode::varnames, NULL);
    code_cls->giveAttrDescriptor("co_flags", BoxedCode::flags, NULL);

    code_cls->freeze();
}
}
