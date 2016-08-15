// Copyright (c) 2014-2016 Dropbox, Inc.
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

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/StringRef.h"

#include "Python.h"

#include "runtime/types.h"

// create a DenseMapInfo which produces the same hash values for llvm::StringRef and BoxedString* keys
namespace llvm {
template <> struct DenseMapInfo<pyston::BoxedString*> {
    static inline pyston::BoxedString* getEmptyKey() {
        uintptr_t Val = static_cast<uintptr_t>(-1);
        Val <<= PointerLikeTypeTraits<pyston::BoxedString*>::NumLowBitsAvailable;
        return reinterpret_cast<pyston::BoxedString*>(Val);
    }
    static inline pyston::BoxedString* getTombstoneKey() {
        uintptr_t Val = static_cast<uintptr_t>(-2);
        Val <<= PointerLikeTypeTraits<pyston::BoxedString*>::NumLowBitsAvailable;
        return reinterpret_cast<pyston::BoxedString*>(Val);
    }

    static unsigned getHashValue(pyston::BoxedString* s) { return pyston::strHashUnboxed(s); }
    static unsigned getHashValue(llvm::StringRef s) { return pyston::strHashUnboxedStrRef(s); }

    static bool isSpecial(pyston::BoxedString* v) { return v == getEmptyKey() || v == getTombstoneKey(); }
    static bool isEqual(pyston::BoxedString* lhs, pyston::BoxedString* rhs) {
        if (isSpecial(lhs) || isSpecial(rhs))
            return lhs == rhs;
        return lhs->s() == rhs->s();
    }
    static bool isEqual(llvm::StringRef lhs, pyston::BoxedString* rhs) {
        if (isSpecial(rhs))
            return false;
        return lhs == rhs->s();
    }
};
}

namespace pyston {
static llvm::DenseSet<BoxedString*> interned_strings;

static StatCounter num_interned_strings("num_interned_string");
extern "C" PyObject* PyString_InternFromString(const char* s) noexcept {
    RELEASE_ASSERT(s, "");
    return internStringImmortal(s);
}

BoxedString* internStringImmortal(llvm::StringRef s) noexcept {
    auto it = interned_strings.find_as(s);
    if (it != interned_strings.end())
        return incref(*it);

    num_interned_strings.log();
    BoxedString* entry = boxString(s);
    // CPython returns mortal but in our current implementation they are inmortal
    entry->interned_state = SSTATE_INTERNED_IMMORTAL;
    interned_strings.insert((BoxedString*)entry);

    Py_INCREF(entry);
    return entry;
}

extern "C" void PyString_InternInPlace(PyObject** p) noexcept {
    BoxedString* s = (BoxedString*)*p;
    if (s == NULL || !PyString_Check(s))
        Py_FatalError("PyString_InternInPlace: strings only please!");
    /* If it's a string subclass, we don't really know what putting
       it in the interned dict might do. */
    if (!PyString_CheckExact(s))
        return;

    if (PyString_CHECK_INTERNED(s))
        return;

    auto it = interned_strings.find(s);
    if (it != interned_strings.end()) {
        auto entry = *it;
        Py_INCREF(entry);
        Py_DECREF(*p);
        *p = entry;
    } else {
        // TODO: do CPython's refcounting here
        num_interned_strings.log();
        interned_strings.insert(s);

        Py_INCREF(s);

        // CPython returns mortal but in our current implementation they are inmortal
        s->interned_state = SSTATE_INTERNED_IMMORTAL;
    }
}

extern "C" void _Py_ReleaseInternedStrings() noexcept {
    // printf("%ld interned strings\n", interned_strings.size());
    for (const auto& p : interned_strings) {
        Py_DECREF(p);
    }
    interned_strings.clear();
}
}
