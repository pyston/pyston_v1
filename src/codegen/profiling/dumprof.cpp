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

#include <sstream>

#include "llvm/ExecutionEngine/JITEventListener.h"
#include "llvm/ExecutionEngine/ObjectImage.h"

#include "codegen/profiling/profiling.h"
#include "core/common.h"
#include "core/options.h"

namespace pyston {

class DumpJITEventListener : public llvm::JITEventListener {
private:
public:
    virtual void NotifyObjectEmitted(const llvm::ObjectImage& Obj);
};

static int num = 0;
void DumpJITEventListener::NotifyObjectEmitted(const llvm::ObjectImage& Obj) {
    llvm::error_code code;

    std::ostringstream os("");
    os << "jit" << ++num << ".o";
    FILE* f = fopen(os.str().c_str(), "w");
    fwrite(Obj.getData().data(), 1, Obj.getData().size(), f);
    fclose(f);
}

llvm::JITEventListener* makeDumpJITEventListener() {
    if (DUMPJIT)
        return new DumpJITEventListener();
    return NULL;
}
static RegisterHelper X(makeDumpJITEventListener);
}
