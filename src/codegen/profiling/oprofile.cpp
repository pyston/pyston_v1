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

#include "opagent.h"

#include "codegen/profiling/profiling.h"
#include "core/common.h"
#include "core/options.h"

namespace pyston {

class OprofileJITEventListener : public llvm::JITEventListener {
private:
    op_agent_t agent;

public:
    OprofileJITEventListener() {
        agent = op_open_agent();
        assert(agent);
    }

    virtual ~OprofileJITEventListener() { op_close_agent(agent); }

    virtual void NotifyObjectEmitted(const llvm::object::ObjectFile& Obj, const llvm::RuntimeDyld::LoadedObjectInfo& L);
};

void OprofileJITEventListener::NotifyObjectEmitted(const llvm::object::ObjectFile& Obj,
                                                   const llvm::RuntimeDyld::LoadedObjectInfo& L) {
    if (VERBOSITY() >= 1)
        printf("An object has been emitted:\n");

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

            if (VERBOSITY() >= 1)
                printf("registering with oprofile: %s %p 0x%lx\n", name.data(), (void*)addr, size);
            int r = op_write_native_code(agent, name.data(), addr, (void*)addr, size);
            assert(r == 0);
            //} else {
            // llvm::StringRef name;
            // code = sym.getName(name);
            // assert(!code);
            // printf("Skipping %s\n", name.data());
        }
    }
}

llvm::JITEventListener* makeOprofileJITEventListener() {
    return new OprofileJITEventListener();
}
static RegisterHelper X(makeOprofileJITEventListener);
}
