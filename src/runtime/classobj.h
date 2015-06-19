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

#ifndef PYSTON_RUNTIME_CLASSOBJ_H
#define PYSTON_RUNTIME_CLASSOBJ_H

#include "runtime/types.h"

namespace pyston {

void setupClassobj();

class BoxedClass;
class BoxedClassobj;
class BoxedInstance;
extern "C" {
extern BoxedClass* classobj_cls, *instance_cls;
}

class BoxedClassobj : public Box {
public:
    HCAttrs attrs;

    BoxedTuple* bases;
    BoxedString* name;

    BoxedClassobj(BoxedString* name, BoxedTuple* bases) : bases(bases), name(name) {}

    static void gcHandler(GCVisitor* v, Box* _o) {
        assert(_o->cls == classobj_cls);
        BoxedClassobj* o = static_cast<BoxedClassobj*>(_o);

        boxGCHandler(v, o);
        if (o->bases)
            v->visit(o->bases);
        if (o->name)
            v->visit(o->name);
    }
};

class BoxedInstance : public Box {
public:
    HCAttrs attrs;

    BoxedClassobj* inst_cls;

    Box** weakreflist;

    BoxedInstance(BoxedClassobj* inst_cls) : inst_cls(inst_cls) {}

    DEFAULT_CLASS(instance_cls);

    static void gcHandler(GCVisitor* v, Box* _o) {
        assert(_o->cls == instance_cls);
        BoxedInstance* o = static_cast<BoxedInstance*>(_o);

        boxGCHandler(v, o);
        if (o->inst_cls)
            v->visit(o->inst_cls);
    }
};
}

#endif
