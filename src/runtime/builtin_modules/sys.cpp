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

#include <cmath>

#include "core/types.h"

#include "runtime/gc_runtime.h"
#include "runtime/types.h"
#include "runtime/util.h"
#include "runtime/inline/boxing.h"

#include "gc/collector.h"

namespace pyston {

BoxedModule* sys_module;
BoxedDict* sys_modules_dict;

BoxedDict* getSysModulesDict() {
    // PyPy's behavior: fetch from sys.modules each time:
    // Box *_sys_modules = sys_module->peekattr("modules");
    // assert(_sys_modules);
    // assert(_sys_modules->cls == dict_cls);
    // return static_cast<BoxedDict*>(_sys_modules);

    // CPython's behavior: return an internalized reference:
    return sys_modules_dict;
}

BoxedList* getSysPath() {
    // Unlike sys.modules, CPython handles sys.path by fetching it each time:
    Box* _sys_path = sys_module->peekattr("path");
    assert(_sys_path);

    if (_sys_path->cls != list_cls) {
        fprintf(stderr, "RuntimeError: sys.path must be a list of directory name\n");
        raiseExcHelper(RuntimeError, "");
    }

    assert(_sys_path->cls == list_cls);
    return static_cast<BoxedList*>(_sys_path);
}

void addToSysArgv(const char* str) {
    Box* sys_argv = sys_module->peekattr("argv");
    assert(sys_argv);
    assert(sys_argv->cls == list_cls);
    listAppendInternal(sys_argv, boxStrConstant(str));
}

void addToSysPath(const std::string& path) {
    BoxedList* sys_path = getSysPath();
    listAppendInternal(sys_path, boxStringPtr(&path));
}

void setupSys() {
    sys_modules_dict = new BoxedDict();
    gc::registerStaticRootObj(sys_modules_dict);

    // This is ok to call here because we've already created the sys_modules_dict
    sys_module = createModule("sys", "__builtin__");

    sys_module->giveAttr("modules", sys_modules_dict);

    BoxedList* sys_path = new BoxedList();
    sys_module->giveAttr("path", sys_path);

    sys_module->giveAttr("argv", new BoxedList());

    sys_module->giveAttr("stdout", new BoxedFile(stdout));
    sys_module->giveAttr("stdin", new BoxedFile(stdin));
    sys_module->giveAttr("stderr", new BoxedFile(stderr));
}
}
