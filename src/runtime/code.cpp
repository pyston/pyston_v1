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

#include <sstream>

#include "Python.h"

#include "code.h"

#include "gc/collector.h"
#include "runtime/objmodel.h"
#include "runtime/set.h"

namespace pyston {

BoxedClass* code_cls;

class BoxedCode : public Box {
public:
    CLFunction* f;

    BoxedCode(CLFunction* f) : f(f) {}

    DEFAULT_CLASS(code_cls);

    static void gcHandler(GCVisitor* v, Box* b) {
        assert(b->cls == code_cls);
        boxGCHandler(v, b);
    }

    static Box* name(Box* b, void*) {
        RELEASE_ASSERT(b->cls == code_cls, "");
        return boxString(static_cast<BoxedCode*>(b)->f->source->getName());
    }

    static Box* filename(Box* b, void*) {
        RELEASE_ASSERT(b->cls == code_cls, "");
        return boxString(static_cast<BoxedCode*>(b)->f->source->parent_module->fn);
    }

    static Box* argcount(Box* b, void*) {
        RELEASE_ASSERT(b->cls == code_cls, "");

        return boxInt(static_cast<BoxedCode*>(b)->f->num_args);
    }

    static Box* varnames(Box* b, void*) {
        RELEASE_ASSERT(b->cls == code_cls, "");
        BoxedCode* code = static_cast<BoxedCode*>(b);

        auto& param_names = code->f->param_names;
        if (!param_names.takes_param_names)
            return EmptyTuple;

        BoxedTuple::GCVector elts;
        for (auto sr : param_names.args)
            elts.push_back(new BoxedString(sr));
        if (param_names.vararg.size())
            elts.push_back(new BoxedString(param_names.vararg));
        if (param_names.kwarg.size())
            elts.push_back(new BoxedString(param_names.kwarg));
        return new BoxedTuple(std::move(elts));
    }

    static Box* flags(Box* b, void*) {
        RELEASE_ASSERT(b->cls == code_cls, "");
        BoxedCode* code = static_cast<BoxedCode*>(b);

        int flags = 0;
        if (code->f->param_names.vararg.size())
            flags |= CO_VARARGS;
        if (code->f->param_names.kwarg.size())
            flags |= CO_VARKEYWORDS;
        return boxInt(flags);
    }
};

Box* codeForFunction(BoxedFunction* f) {
    return new BoxedCode(f->f);
}

Box* codeForCLFunction(CLFunction* f) {
    return new BoxedCode(f);
}

void setupCode() {
    code_cls
        = BoxedHeapClass::create(type_cls, object_cls, &BoxedCode::gcHandler, 0, 0, sizeof(BoxedCode), false, "code");

    code_cls->giveAttr("__new__", None); // Hacky way of preventing users from instantiating this

    code_cls->giveAttr("co_name", new (pyston_getset_cls) BoxedGetsetDescriptor(BoxedCode::name, NULL, NULL));
    code_cls->giveAttr("co_filename", new (pyston_getset_cls) BoxedGetsetDescriptor(BoxedCode::filename, NULL, NULL));
    code_cls->giveAttr("co_argcount", new (pyston_getset_cls) BoxedGetsetDescriptor(BoxedCode::argcount, NULL, NULL));
    code_cls->giveAttr("co_varnames", new (pyston_getset_cls) BoxedGetsetDescriptor(BoxedCode::varnames, NULL, NULL));
    code_cls->giveAttr("co_flags", new (pyston_getset_cls) BoxedGetsetDescriptor(BoxedCode::flags, NULL, NULL));

    code_cls->freeze();
}
}
