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

#include "llvm/ADT/Hashing.h"
#include "llvm/ADT/StringRef.h"

#include "core/common.h"

namespace std {
template <> struct hash<llvm::StringRef> {
    size_t operator()(const llvm::StringRef s) const { return llvm::hash_value(s); }
};
}

namespace pyston {

class InternedStringPool;
class InternedString {
private:
    const std::string* _str;

#ifndef NDEBUG
    // Only for testing purposes:
    InternedStringPool* pool;
    InternedString(const std::string* str, InternedStringPool* pool) : _str(str), pool(pool) {}
#else
    InternedString(const std::string* str) : _str(str) {}
#endif

public:
#ifndef NDEBUG
    InternedString() : _str(NULL), pool(NULL) {}
#else
    InternedString() : _str(NULL) {}
#endif

    // operator const std::string&() { return *_str; }
    const std::string& str() const {
        assert(this->_str);
        return *_str;
    }

    const char* c_str() const {
        assert(this->_str);
        return _str->c_str();
    }

    bool operator==(InternedString rhs) const {
        assert(this->_str);
        assert(this->pool == rhs.pool);
        return this->_str == rhs._str;
    }

    bool operator<(InternedString rhs) const {
        assert(this->_str);
        assert(this->pool == rhs.pool);
        return this->_str < rhs._str;
    }

    friend class InternedStringPool;
    friend struct std::hash<InternedString>;
    friend struct std::less<InternedString>;
};

class InternedStringPool {
private:
    // We probably don't need to pull in llvm::StringRef as the key, but it's better than std::string
    // which I assume forces extra allocations.
    // (We could define a custom string-pointer container but is it worth it?)
    std::unordered_map<llvm::StringRef, std::string*> interned;

public:
    ~InternedStringPool() {
        for (auto& p : interned) {
            delete p.second;
        }
    }

    template <class T> InternedString get(T&& arg) {
        auto it = interned.find(llvm::StringRef(arg));

        std::string* s;
        if (it != interned.end()) {
            s = it->second;
        } else {
            s = new std::string(std::forward<T>(arg));
            interned.insert(it, std::make_pair(llvm::StringRef(*s), s));
        }

#ifndef NDEBUG
        return InternedString(s, this);
#else
        return InternedString(s);
#endif
    }
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
        return *lhs._str < *rhs._str;
    }
};
}

#endif
