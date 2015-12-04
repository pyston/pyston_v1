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

#ifndef PYSTON_CORE_STRINGPOOL_H
#define PYSTON_CORE_STRINGPOOL_H

#include <algorithm>
#include <cstdio>
#include <sys/time.h>

#include "llvm/ADT/DenseMapInfo.h"
#include "llvm/ADT/Hashing.h"
#include "llvm/ADT/StringRef.h"

#include "core/common.h"

namespace std {
template <> struct hash<llvm::StringRef> {
    size_t operator()(const llvm::StringRef s) const { return llvm::hash_value(s); }
};
}

namespace pyston {

class BoxedString;

namespace gc {
class GCVisitor;
}

class InternedStringPool;
class InternedString {
private:
    BoxedString* _str;

#ifndef NDEBUG
    // Only for testing purposes:
    InternedStringPool* pool;
    InternedString(BoxedString* str, InternedStringPool* pool) : _str(str), pool(pool) {}

    static InternedStringPool* invalidPool() { return reinterpret_cast<InternedStringPool*>(-1); }
#else
    InternedString(BoxedString* str) : _str(str) {}
#endif

public:
#ifndef NDEBUG
    InternedString() : _str(NULL), pool(NULL) {}
#else
    InternedString() : _str(NULL) {}
#endif

    BoxedString* getBox() const {
        assert(this->_str);
        return _str;
    }

    const char* c_str() const;

    bool operator==(InternedString rhs) const {
        assert(this->_str || this->pool == invalidPool());
        assert(rhs._str || rhs.pool == invalidPool());
        assert(this->pool == rhs.pool || this->pool == invalidPool() || rhs.pool == invalidPool());
        return this->_str == rhs._str;
    }

    // This function compares the actual string contents
    bool operator<(InternedString rhs) const { return this->s().compare(rhs.s()) == -1; }

    llvm::StringRef s() const;
    operator llvm::StringRef() const { return s(); }
    operator BoxedString*() const { return getBox(); }

    bool isCompilerCreatedName() const;

    friend class InternedStringPool;
    friend struct std::hash<InternedString>;
    friend struct std::less<InternedString>;
    friend struct llvm::DenseMapInfo<pyston::InternedString>;
};

class InternedStringPool {
public:
    void gcHandler(gc::GCVisitor* v);
    InternedString get(llvm::StringRef s);
};

} // namespace pyston

namespace std {
template <> struct hash<pyston::InternedString> {
    size_t operator()(const pyston::InternedString s) const { return reinterpret_cast<intptr_t>(s._str) >> 3; }
};
template <> struct less<pyston::InternedString> {
    bool operator()(const pyston::InternedString lhs, const pyston::InternedString rhs) const {
        assert(lhs.pool && lhs.pool == rhs.pool);
        // TODO: we should be able to do this comparison on the pointer value, not on the string value,
        // but there are apparently parts of the code that rely on string sorting being actually alphabetical.
        // We could create a faster "consistent ordering but not alphabetical" comparator if it makes a difference.
        return lhs < rhs;
    }
};
}

namespace llvm {
template <> struct DenseMapInfo<pyston::InternedString> {
    static inline pyston::InternedString getEmptyKey() {
#ifndef NDEBUG
        return pyston::InternedString(nullptr, pyston::InternedString::invalidPool());
#else
        return pyston::InternedString(nullptr);
#endif
    }
    static inline pyston::InternedString getTombstoneKey() {
#ifndef NDEBUG
        return pyston::InternedString((pyston::BoxedString*)-1, pyston::InternedString::invalidPool());
#else
        return pyston::InternedString((pyston::BoxedString*)-1);
#endif
    }
    static unsigned getHashValue(const pyston::InternedString& val) { return std::hash<pyston::InternedString>()(val); }
    static bool isEqual(const pyston::InternedString& lhs, const pyston::InternedString& rhs) { return lhs == rhs; }
};
}

#endif
