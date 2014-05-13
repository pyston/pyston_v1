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

#ifndef PYSTON_CODEGEN_GCBUILDER_H
#define PYSTON_CODEGEN_GCBUILDER_H

namespace llvm {
class Value;
}

namespace pyston {

class IREmitter;
class GCBuilder {
public:
    virtual ~GCBuilder() {}

    virtual llvm::Value* readPointer(IREmitter&, llvm::Value* ptr_ptr) = 0;
    virtual void writePointer(IREmitter&, llvm::Value* ptr_ptr, llvm::Value* ptr_value, bool ignore_existing_value) = 0;

    virtual void grabPointer(IREmitter&, llvm::Value* ptr) = 0;
    virtual void dropPointer(IREmitter&, llvm::Value* ptr) = 0;
};

GCBuilder* getGCBuilder();
}

#endif
