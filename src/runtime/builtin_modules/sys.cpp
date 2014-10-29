// Copyright (c) 2014 Dropbox, Inc.
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

#include <algorithm>
#include <cmath>

#include "core/types.h"
#include "gc/collector.h"
#include "runtime/inline/boxing.h"
#include "runtime/int.h"
#include "runtime/types.h"
#include "runtime/util.h"

namespace pyston {

BoxedModule* sys_module;
BoxedDict* sys_modules_dict;

BoxedDict* getSysModulesDict() {
    // PyPy's behavior: fetch from sys.modules each time:
    // Box *_sys_modules = sys_module->getattr("modules");
    // assert(_sys_modules);
    // assert(_sys_modules->cls == dict_cls);
    // return static_cast<BoxedDict*>(_sys_modules);

    // CPython's behavior: return an internalized reference:
    return sys_modules_dict;
}

BoxedList* getSysPath() {
    // Unlike sys.modules, CPython handles sys.path by fetching it each time:
    Box* _sys_path = sys_module->getattr("path");
    assert(_sys_path);

    if (_sys_path->cls != list_cls) {
        fprintf(stderr, "RuntimeError: sys.path must be a list of directory name\n");
        raiseExcHelper(RuntimeError, "");
    }

    assert(_sys_path->cls == list_cls);
    return static_cast<BoxedList*>(_sys_path);
}

Box* getSysStdout() {
    Box* sys_stdout = sys_module->getattr("stdout");
    RELEASE_ASSERT(sys_stdout, "lost sys.stdout??");
    return sys_stdout;
}

void addToSysArgv(const char* str) {
    Box* sys_argv = sys_module->getattr("argv");
    assert(sys_argv);
    assert(sys_argv->cls == list_cls);
    listAppendInternal(sys_argv, boxStrConstant(str));
}

void appendToSysPath(const std::string& path) {
    BoxedList* sys_path = getSysPath();
    listAppendInternal(sys_path, boxStringPtr(&path));
}

void prependToSysPath(const std::string& path) {
    BoxedList* sys_path = getSysPath();
    static std::string attr = "insert";
    callattr(sys_path, &attr, false, ArgPassSpec(2), boxInt(0), new BoxedString(path), NULL, NULL, NULL);
}

static BoxedClass* sys_flags_cls;
class BoxedSysFlags : public Box {
public:
    Box* division_warning, *bytes_warning;

    BoxedSysFlags() : Box(sys_flags_cls) {
        auto zero = boxInt(0);
        division_warning = zero;
        bytes_warning = zero;
    }

    static void gcHandler(GCVisitor* v, Box* b) {
        assert(b->cls == sys_flags_cls);

        boxGCHandler(v, b);
    }

    static Box* __new__(Box* cls, Box* args, Box* kwargs) {
        raiseExcHelper(TypeError, "cannot create 'sys.flags' instances");
    }
};

void setupSys() {
    sys_modules_dict = new BoxedDict();
    gc::registerPermanentRoot(sys_modules_dict);

    // This is ok to call here because we've already created the sys_modules_dict
    sys_module = createModule("sys", "__builtin__");

    sys_module->giveAttr("modules", sys_modules_dict);

    BoxedList* sys_path = new BoxedList();
    sys_module->giveAttr("path", sys_path);

    sys_module->giveAttr("argv", new BoxedList());

    sys_module->giveAttr("stdout", new BoxedFile(stdout, "<stdout>", "w"));
    sys_module->giveAttr("stdin", new BoxedFile(stdin, "<stdin>", "r"));
    sys_module->giveAttr("stderr", new BoxedFile(stderr, "<stderr>", "w"));

    sys_module->giveAttr("warnoptions", new BoxedList());
    sys_module->giveAttr("py3kwarning", False);

    sys_module->giveAttr("platform", boxStrConstant("unknown")); // seems like a reasonable, if poor, default

    sys_module->giveAttr("hexversion", boxInt(PY_VERSION_HEX));

    sys_module->giveAttr("maxint", boxInt(PYSTON_INT_MAX));

    sys_flags_cls = new BoxedClass(type_cls, object_cls, BoxedSysFlags::gcHandler, 0, sizeof(BoxedSysFlags), false);
    sys_flags_cls->giveAttr("__name__", boxStrConstant("flags"));
    sys_flags_cls->giveAttr("__new__",
                            new BoxedFunction(boxRTFunction((void*)BoxedSysFlags::__new__, UNKNOWN, 1, 0, true, true)));
#define ADD(name)                                                                                                      \
    sys_flags_cls->giveAttr(STRINGIFY(name),                                                                           \
                            new BoxedMemberDescriptor(BoxedMemberDescriptor::OBJECT, offsetof(BoxedSysFlags, name)))
    ADD(division_warning);
    ADD(bytes_warning);
#undef ADD

    sys_flags_cls->freeze();

    sys_module->giveAttr("flags", new BoxedSysFlags());
}

void setupSysEnd() {
    BoxedTuple::GCVector builtin_module_names;
    for (auto& p : sys_modules_dict->d) {
        builtin_module_names.push_back(p.first);
    }

    std::sort<decltype(builtin_module_names)::iterator, PyLt>(builtin_module_names.begin(), builtin_module_names.end(),
                                                              PyLt());

    sys_module->giveAttr("builtin_module_names", new BoxedTuple(std::move(builtin_module_names)));
}
}
