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

#ifndef PYSTON_CODEGEN_UNWINDING_H
#define PYSTON_CODEGEN_UNWINDING_H

#include <unordered_map>

#include "codegen/codegen.h"

namespace pyston {

std::vector<const LineInfo*> getTracebackEntries();
const LineInfo* getMostRecentLineInfo();
class BoxedModule;
BoxedModule* getCurrentModule();

CompiledFunction* getCFForAddress(uint64_t addr);
class BoxedDict;
BoxedDict* getLocals(bool only_user_visible);

// Fetches the frame-local excinfo object, calculating it if necessary (from previous frames):
ExcInfo getFrameExcInfo();
}

#endif
