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

#include "llvm/ExecutionEngine/JITEventListener.h"
#include "llvm/Object/ObjectFile.h"

#include "codegen/profiling/profiling.h"
#include "core/common.h"
#include "core/options.h"

namespace pyston {

class PprofJITEventListener : public llvm::JITEventListener {
private:
    FILE* of;

public:
    PprofJITEventListener() { of = fopen("pprof.jit", "w"); }
    virtual ~PprofJITEventListener() { fclose(of); }

    virtual void NotifyObjectEmitted(const llvm::object::ObjectFile& Obj, const llvm::RuntimeDyld::LoadedObjectInfo& L);
};

void PprofJITEventListener::NotifyObjectEmitted(const llvm::object::ObjectFile& Obj,
                                                const llvm::RuntimeDyld::LoadedObjectInfo& L) {
    llvm_error_code code;
    for (const auto& sym : Obj.symbols()) {
        llvm::object::SymbolRef::Type type;
        code = sym.getType(type);
        assert(!code);
        if (type == llvm::object::SymbolRef::ST_Function) {
            llvm::StringRef name;
            uint64_t addr, size;
            code = sym.getName(name);
            assert(!code);
            if (name.empty())
                continue;
            addr = g.engine->getGlobalValueAddress(name);
            code = sym.getSize(size);
            assert(!code);

            // fprintf(of, "%lx-%lx: %s\n", addr, addr + size, name.data());
            // if (VERBOSITY() >= 1)
            // printf("%lx-%lx: %s\n", addr, addr + size, name.data());
            fprintf(of, "%lx %lx %s\n", addr, addr + size, name.data());
            if (VERBOSITY() >= 1)
                printf("%lx %lx %s\n", addr, addr + size, name.data());
        }
    }
}

llvm::JITEventListener* makePprofJITEventListener() {
    return new PprofJITEventListener();
}
static RegisterHelper X(makePprofJITEventListener);
}
