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

#ifndef PYSTON_CODEGEN_STACKMAPS_H
#define PYSTON_CODEGEN_STACKMAPS_H

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"

namespace llvm {
class JITEventListener;
}

namespace pyston {

class CompilerType;

struct StackMap {
    struct __attribute__((__packed__)) StackSizeRecord {
        uint64_t offset;
        uint64_t stack_size;
    };

    struct Record {
        struct __attribute__((__packed__)) Location {
            enum LocationType : uint8_t {
                Register = 0x1,
                Direct = 0x2,
                Indirect = 0x3,
                Constant = 0x4,
                ConstIndex = 0x5,
            } type;

            uint8_t flags;
            uint16_t regnum;
            int32_t offset;

            bool operator==(const Location& rhs);
        };

        struct __attribute__((__packed__)) LiveOut {
            uint16_t regnum;
            uint8_t reserved;
            uint8_t size;
        };

        uint64_t id;
        uint32_t offset;
        uint16_t flags;
        llvm::SmallVector<Location, 8> locations;
        llvm::SmallVector<LiveOut, 8> live_outs;
    };

    llvm::SmallVector<StackSizeRecord, 1> stack_size_records;
    uint32_t header;
    llvm::SmallVector<uint64_t, 8> constants;
    std::vector<Record> records;
};

// TODO this belongs somewhere else?
class LocationMap {
public:
    llvm::SmallVector<uint64_t, 8> constants;

    StackMap::Record::Location frame_info_location;
    bool frameInfoFound() { return frame_info_location.type != 0; }

    struct LocationTable {
        struct LocationEntry {
            uint64_t _debug_pp_id;

            unsigned offset;
            int length;
            CompilerType* type;
            llvm::SmallVector<StackMap::Record::Location, 1> locations;
        };
        llvm::SmallVector<LocationEntry, 2> locations;

        const LocationEntry* findEntry(unsigned offset) const;
    };

    llvm::StringMap<LocationTable> names;
};

StackMap* parseStackMap();
llvm::JITEventListener* makeStackMapListener();
}

#endif
