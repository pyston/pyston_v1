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

#ifndef PYSTON_CORE_THREADING_H
#define PYSTON_CORE_THREADING_H

namespace pyston {
namespace threading {

#define THREADING_USE_GIL 0
#define THREADING_SAFE_DATASTRUCTURES 0

void ensureSerial();
void endEnsureSerial();

#if THREADING_USE_GIL
inline void ensureSerial() {
}
inline void endEnsureSerial() {
}
#endif

class SerialRegion {
public:
    SerialRegion() { ensureSerial(); }
    ~SerialRegion() { endEnsureSerial(); }
};

class UnSerialRegion {
public:
    UnSerialRegion() { endEnsureSerial(); }
    ~UnSerialRegion() { ensureSerial(); }
};

void allowThreads();
void endAllowThreads();
class AllowThreadsRegion {
public:
    AllowThreadsRegion() { allowThreads(); }
    ~AllowThreadsRegion() { endAllowThreads(); }
};

} // namespace threading
} // namespace pyston

#endif
