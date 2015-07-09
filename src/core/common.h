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

#ifndef PYSTON_CORE_COMMON_H
#define PYSTON_CORE_COMMON_H

#include <cassert>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <stdint.h>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#define STACK_GROWS_DOWN 1

#ifndef __has_builtin
#define __has_builtin(x) 0
#endif

#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

#define _STRINGIFY(N) #N
#define STRINGIFY(N) _STRINGIFY(N)
#define _CAT(A, B) A##B
#define CAT(A, B) _CAT(A, B)

#define ARRAY_LEN(arr) (sizeof(arr) / sizeof((arr)[0]))

#if LLVMREV < 210783
#define llvm_error_code llvm::error_code
#else
#define llvm_error_code std::error_code
#endif

// From http://stackoverflow.com/questions/3767869/adding-message-to-assert, modified to use fprintf and give a Python
// stacktrace
#define RELEASE_ASSERT(condition, fmt, ...)                                                                            \
    do {                                                                                                               \
        if (!(condition)) {                                                                                            \
            ::fprintf(stderr, __FILE__ ":" STRINGIFY(__LINE__) ": %s: Assertion `" #condition "' failed: " fmt "\n",   \
                      __PRETTY_FUNCTION__, ##__VA_ARGS__);                                                             \
            ::abort();                                                                                                 \
        }                                                                                                              \
    } while (false)
#ifndef NDEBUG
#define ASSERT RELEASE_ASSERT
#else
#define ASSERT(condition, fmt, ...)                                                                                    \
    do {                                                                                                               \
    } while (false)
#endif

#define UNIMPLEMENTED() RELEASE_ASSERT(0, "unimplemented")

// Allow using std::pair as keys in hashtables:
namespace std {
template <typename T1, typename T2> struct hash<pair<T1, T2>> {
    size_t operator()(const pair<T1, T2> p) const { return hash<T1>()(p.first) ^ (hash<T2>()(p.second) << 1); }
};
}

namespace std {
template <typename T1, typename T2, typename T3> struct hash<tuple<T1, T2, T3>> {
    size_t operator()(const tuple<T1, T2, T3> p) const {
        return hash<T1>()(std::get<0>(p)) ^ (hash<T2>()(std::get<1>(p)) << 1) ^ (hash<T3>()(std::get<2>(p)) << 2);
    }
};
}

#endif
