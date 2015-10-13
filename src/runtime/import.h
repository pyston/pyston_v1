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

#ifndef PYSTON_RUNTIME_IMPORT_H
#define PYSTON_RUNTIME_IMPORT_H

#include "runtime/types.h"

namespace pyston {

extern "C" PyObject* PyImport_GetImporter(PyObject* path) noexcept;
extern "C" Box* import(int level, Box* from_imports, llvm::StringRef module_name);
extern Box* importModuleLevel(llvm::StringRef module_name, Box* globals, Box* from_imports, int level);
BoxedModule* importCExtension(BoxedString* full_name, const std::string& last_name, const std::string& path);
}

#endif
