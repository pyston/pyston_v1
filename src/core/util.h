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

#ifndef PYSTON_CORE_UTIL_H
#define PYSTON_CORE_UTIL_H

#include <algorithm>
#include <cstdio>
#include <sys/time.h>

#include "core/common.h"
#include "core/stringpool.h"

namespace pyston {

inline uint64_t getCPUTicks() {
    unsigned long lo, hi;
    asm("rdtsc" : "=a"(lo), "=d"(hi));
    return (lo | (hi << 32));
}

#define DISABLE_TIMERS 0

#if !DISABLE_TIMERS
class Timer {
private:
    static int level;
    uint64_t start_time;
    const char* desc;
    long min_usec;
    bool ended;
    std::function<void(uint64_t)> exit_callback;

public:
    // Timers with non-NULL desc will print times longer than min_usec for debugging when VERBOSITY("time") >= 2
    Timer(const char* desc = NULL, long min_usec = -1);
    Timer(long min_usec); // doesn't start the timer
    ~Timer();

    void setExitCallback(std::function<void(uint64_t)> _exit_callback) { exit_callback = _exit_callback; }

    void restart(const char* newdesc, long new_min_usec);
    void restart(const char* newdesc = NULL);

    // returns the duration.  if @ended_at is non-null, it's filled in
    // with the tick the timer stopped at.
    uint64_t end(uint64_t* ended_at = NULL);
    uint64_t split(const char* newdesc = NULL) {
        uint64_t rtn = end();
        restart(newdesc);
        return rtn;
    }

    uint64_t getStartTime() const { return start_time; }
};

#else // DISABLE_TIMERS
class Timer {
public:
    Timer(const char* desc = NULL, long min_usec = -1) {}
    Timer(long min_usec) {}

    void setExitCallback(std::function<void(uint64_t)> _exit_callback) {}

    void restart(const char* newdesc, long new_min_usec) {}
    void restart(const char* newdesc = NULL) {}

    long end() { return 0; }
    long split(const char* newdesc = NULL) { return 0; }
};

#endif // #else DISABLE_TIMERS

inline bool startswith(llvm::StringRef s, llvm::StringRef pattern) {
    return s.startswith(pattern);
}

inline bool endswith(llvm::StringRef s, llvm::StringRef pattern) {
    return s.endswith(pattern);
}

void removeDirectoryIfExists(const std::string& path);

// Checks that lhs and rhs, which are iterables of InternedStrings, have the
// same set of names in them.
template <class T1, class T2> bool sameKeyset(T1* lhs, T2* rhs) {
    std::vector<int> lv, rv;
    for (typename T1::iterator it = lhs->begin(); it != lhs->end(); ++it) {
        lv.push_back((*it).first);
    }
    for (typename T2::iterator it = rhs->begin(); it != rhs->end(); ++it) {
        rv.push_back((*it).first);
    }

    std::sort(lv.begin(), lv.end());
    std::sort(rv.begin(), rv.end());

    std::vector<int> lextra(lv.size());
    std::vector<int>::iterator diffend
        = std::set_difference(lv.begin(), lv.end(), rv.begin(), rv.end(), lextra.begin());
    lextra.resize(diffend - lextra.begin());

    bool good = true;
    if (lextra.size()) {
        printf("Only in lhs:\n");
        for (int i = 0; i < lextra.size(); i++) {
            printf("%d\n", lextra[i]);
        }
        good = false;
    }

    std::vector<int> rextra(rv.size());
    diffend = std::set_difference(rv.begin(), rv.end(), lv.begin(), lv.end(), rextra.begin());
    rextra.resize(diffend - rextra.begin());

    if (rextra.size()) {
        printf("Only in rhs:\n");
        for (int i = 0; i < rextra.size(); i++) {
            printf("%d\n", rextra[i]);
        }
        good = false;
    }
    return good;
}

// A simple constant-width bitset.
template <int N> struct BitSet {
    uint16_t bits = 0;
    static_assert(N <= 8 * sizeof(bits), "");

    struct iterator {
        const BitSet& set;
        int cur;

        iterator(const BitSet& set, int cur) : set(set), cur(cur) {}

        int operator*() {
            assert(cur >= 0 && cur < N);
            return cur;
        }

        bool operator==(const iterator& rhs) { return cur == rhs.cur; }
        bool operator!=(const iterator& rhs) { return !(*this == rhs); }
        iterator& operator++() {
            assert(cur >= 0 && cur < N);
            uint16_t tmp = set.bits;
            tmp >>= cur + 1;
            cur++;
            if (tmp > 0) {
                int offset = __builtin_ctz(tmp);
                if (cur + offset < N) {
                    cur += offset;
                    return *this;
                }
            }
            cur = N;
            assert(cur == N);
            return *this;
        }
    };

    void set(int idx) {
        assert(idx >= 0 && idx < N);
        bits |= (1 << idx);
    }
    void clear(int idx) {
        assert(idx >= 0 && idx < N);
        bits &= ~(1 << idx);
    }
    iterator begin() const {
        uint16_t tmp = bits;
        if (tmp > 0) {
            int offset = __builtin_ctz(tmp);
            if (offset < N)
                return iterator(*this, offset);
        }
        return iterator(*this, N);
    }
    iterator end() const { return iterator(*this, N); }
};
}

#endif
