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

#ifndef PYSTON_CODEGEN_STACKMAPS_H
#define PYSTON_CODEGEN_STACKMAPS_H

#include <cstdint>
#include <vector>

namespace llvm {
class JITEventListener;
}

namespace pyston {

struct StackMap {
    struct __attribute__((__packed__)) StackSizeRecord {
        uint64_t offset;
        uint64_t stack_size;
    };

    struct Record {
        struct __attribute__((__packed__)) Location {
            uint8_t type;
            uint8_t flags;
            uint16_t regnum;
            int32_t offset;
        };

        struct __attribute__((__packed__)) LiveOut {
            uint16_t regnum;
            uint8_t reserved;
            uint8_t size;
        };

        uint64_t id;
        uint32_t offset;
        uint16_t flags;
        std::vector<Location> locations;
        std::vector<LiveOut> live_outs;
    };

    std::vector<StackSizeRecord> stack_size_records;
    uint32_t header;
    std::vector<uint64_t> constants;
    std::vector<Record*> records;
};

StackMap* parseStackMap();
llvm::JITEventListener* makeStackMapListener();
}

#endif
