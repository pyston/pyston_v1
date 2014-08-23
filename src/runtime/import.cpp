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

#include "runtime/import.h"

#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"

#include "codegen/irgen/hooks.h"
#include "codegen/parser.h"
#include "runtime/capi.h"
#include "runtime/objmodel.h"

namespace pyston {

BoxedModule* compileAndRunModule(const std::string& name, const std::string& fn, bool add_to_sys_modules) {
    BoxedModule* module = createModule(name, fn, add_to_sys_modules);

    AST_Module* ast = caching_parse(fn.c_str());
    compileAndRunModule(ast, module);
    return module;
}

#if LLVMREV < 210072
#define LLVM_SYS_FS_EXISTS_CODE_OKAY(code) ((code) == 0)
#else
#define LLVM_SYS_FS_EXISTS_CODE_OKAY(code) (!(code))
#endif

extern "C" Box* import(const std::string* name) {
    assert(name);

    static StatCounter slowpath_import("slowpath_import");
    slowpath_import.log();

    Box* parent_module = NULL;
    size_t periodIndex = name->rfind('.');
    if (periodIndex != std::string::npos) {
        std::string parent_name(*name, 0, periodIndex);
        parent_module = import(&parent_name);
    }

    BoxedDict* sys_modules = getSysModulesDict();
    Box* s = boxStringPtr(name);
    if (sys_modules->d.find(s) != sys_modules->d.end())
        return sys_modules->d[s];

    BoxedList* path_list;
    if (parent_module == NULL) {
        path_list = getSysPath();
        if (path_list == NULL || path_list->cls != list_cls) {
            raiseExcHelper(RuntimeError, "sys.path must be a list of directory names");
        }
    } else {
        path_list = static_cast<BoxedList*>(parent_module->getattr("__path__", NULL));
        if (path_list == NULL || path_list->cls != list_cls) {
            raiseExcHelper(ImportError, "No module named %s", name->c_str());
        }
    }

    llvm::SmallString<128> joined_path;
    for (int i = 0; i < path_list->size; i++) {
        Box* _p = path_list->elts->elts[i];
        if (_p->cls != str_cls)
            continue;
        BoxedString* p = static_cast<BoxedString*>(_p);

        joined_path.clear();
        llvm::sys::path::append(joined_path, p->s, *name + ".py");
        std::string fn(joined_path.str());

        if (VERBOSITY() >= 2)
            printf("Searching for %s at %s...\n", name->c_str(), fn.c_str());

        bool exists;
        llvm::error_code code = llvm::sys::fs::exists(joined_path.str(), exists);
        assert(LLVM_SYS_FS_EXISTS_CODE_OKAY(code));

        if (!exists)
            continue;

        if (VERBOSITY() >= 1)
            printf("Importing %s from %s\n", name->c_str(), fn.c_str());

        // TODO duplication with jit.cpp:
        BoxedModule* module = compileAndRunModule(*name, fn, true);
        return module;
    }

    if (*name == "test") {
        return importTestExtension();
    }

    raiseExcHelper(ImportError, "No module named %s", name->c_str());
}
}
