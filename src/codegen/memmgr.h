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

#ifndef PYSTON_CODEGEN_MEMMGR_H
#define PYSTON_CODEGEN_MEMMGR_H

#include <cstddef>
#include <cstdint>

namespace llvm {
class RTDyldMemoryManager;
}

namespace pyston {

llvm::RTDyldMemoryManager* createMemoryManager();
void registerEHFrames(uint8_t* addr, uint64_t load_addr, size_t size);
void deregisterEHFrames(uint8_t* addr, uint64_t load_addr, size_t size);
}

#endif
