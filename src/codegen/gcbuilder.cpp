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

#include "codegen/gcbuilder.h"

#include "codegen/irgen.h"
#include "core/options.h"

namespace pyston {

class ConservativeGCBuilder : public GCBuilder {
private:
    virtual llvm::Value* readPointer(IREmitter& emitter, llvm::Value* ptr_ptr) {
        assert(ptr_ptr->getType() == g.llvm_value_type_ptr);
        return emitter.getBuilder()->CreateLoad(ptr_ptr);
    }
    virtual void writePointer(IREmitter& emitter, llvm::Value* ptr_ptr, llvm::Value* ptr_value,
                              bool ignore_existing_value) {
        assert(ptr_ptr->getType() == g.llvm_value_type_ptr);
        emitter.getBuilder()->CreateStore(ptr_value, ptr_ptr);
    }
    virtual void grabPointer(IREmitter& emitter, llvm::Value* ptr) {}
    virtual void dropPointer(IREmitter& emitter, llvm::Value* ptr) {}
};

ConservativeGCBuilder cgc_builder;

GCBuilder* getGCBuilder() {
    return &cgc_builder;
}
}
