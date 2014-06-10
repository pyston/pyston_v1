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

#include "llvm/ExecutionEngine/JITEventListener.h"
#include "llvm/ExecutionEngine/ObjectImage.h"

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

    virtual void NotifyObjectEmitted(const llvm::ObjectImage& Obj);
};

void PprofJITEventListener::NotifyObjectEmitted(const llvm::ObjectImage& Obj) {
    llvm::error_code code;
    for (llvm::object::symbol_iterator I = Obj.begin_symbols(), E = Obj.end_symbols(); I != E;) {
        llvm::object::SymbolRef::Type type;
        code = I->getType(type);
        assert(!code);
        if (type == llvm::object::SymbolRef::ST_Function) {
            llvm::StringRef name;
            uint64_t addr, size;
            code = I->getName(name);
            assert(!code);
            code = I->getAddress(addr);
            assert(!code);
            code = I->getSize(size);
            assert(!code);

            // fprintf(of, "%lx-%lx: %s\n", addr, addr + size, name.data());
            // if (VERBOSITY() >= 1)
            // printf("%lx-%lx: %s\n", addr, addr + size, name.data());
            fprintf(of, "%lx %lx %s\n", addr, addr + size, name.data());
            if (VERBOSITY() >= 1)
                printf("%lx %lx %s\n", addr, addr + size, name.data());
        }
        ++I;
    }
}

llvm::JITEventListener* makePprofJITEventListener() {
    return new PprofJITEventListener();
}
static RegisterHelper X(makePprofJITEventListener);
}
