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

#include "core/stringpool.h"

#include "runtime/types.h"

namespace pyston {

InternedString InternedStringPool::get(llvm::StringRef arg) {
    // HACK: should properly track this liveness:
    BoxedString* s = internStringImmortal(arg);

#ifndef NDEBUG
    return InternedString(s, this);
#else
    return InternedString(s);
#endif
}

llvm::StringRef InternedString::s() const {
    return _str->s();
}

const char* InternedString::c_str() const {
    return _str->c_str();
}
}
