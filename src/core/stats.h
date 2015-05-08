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

#ifndef PYSTON_CORE_STATS_H
#define PYSTON_CORE_STATS_H

#include <atomic>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/options.h"
#include "core/util.h"

namespace pyston {

#define DISABLE_STATS 0

#define STAT_ALLOCATIONS 0 && !DISABLE_STATS
#define STAT_TIMERS 0 && !DISABLE_STATS

#if STAT_TIMERS
#define STAT_TIMER(id, name)                                                                                           \
    static int _stid##id = Stats::getStatId(name);                                                                     \
    StatTimer _st##id(_stid##id)
#define STAT_TIMER2(id, name, at_time)                                                                                 \
    static int _stid##id = Stats::getStatId(name);                                                                     \
    StatTimer _st##id(_stid##id, at_time)
#else
#define STAT_TIMER(id, name) StatTimer _st##id(0);
#define STAT_TIMER2(id, name, at_time) StatTimer _st##id(0);
#endif

#define STAT_TIMER_NAME(id) _st##id

#if !DISABLE_STATS
struct Stats {
private:
    static std::vector<uint64_t>* counts;
    static std::unordered_map<int, std::string>* names;
    static bool enabled;

    static timespec start_ts;
    static uint64_t start_tick;

public:
    static void startEstimatingCPUFreq();
    static double estimateCPUFreq();

    static int getStatId(const std::string& name);
    static std::string getStatName(int id);

    static void setEnabled(bool enabled) { Stats::enabled = enabled; }
    static void log(int id, uint64_t count = 1) { (*counts)[id] += count; }

    static void clear() { std::fill(counts->begin(), counts->end(), 0); }
    static void dump(bool includeZeros = true);
    static void endOfInit();
};

struct StatCounter {
private:
    int id;

public:
    StatCounter(const std::string& name);

    void log(uint64_t count = 1) { Stats::log(id, count); }
};

struct StatPerThreadCounter {
private:
    int id = 0;

public:
    StatPerThreadCounter(const std::string& name);

    void log(uint64_t count = 1) { Stats::log(id, count); }
};

#else
struct Stats {
    static void startEstimatingCPUFreq() {}
    static double estimateCPUFreq() { return 0; }
    static void setEnabled(bool enabled) {}
    static void dump(bool includeZeros = true) { printf("(Stats disabled)\n"); }
    static void clear() {}
    static void log(int id, int count = 1) {}
    static int getStatId(const std::string& name) { return 0; }
    static void endOfInit() {}
};
struct StatCounter {
    StatCounter(const char* name) {}
    void log(uint64_t count = 1){};
};
struct StatPerThreadCounter {
    StatPerThreadCounter(const char* name) {}
    void log(uint64_t count = 1){};
};
#endif

#if STAT_TIMERS
class StatTimer {
private:
    static __thread StatTimer* stack;

    // the start time of the current active segment (0 == paused)
    uint64_t _start_time;

    uint64_t _last_pause_time;

    StatTimer* _prev;

    int _statid;

public:
    StatTimer(int statid, bool push = true);
    StatTimer(int statid, uint64_t at_time);
    ~StatTimer();

    void pause(uint64_t at_time);
    void resume(uint64_t at_time);

    bool isPaused() const { return _start_time == 0; }
    int getId() const { return _statid; }

    static StatTimer* getStack() { return stack; }

    static StatTimer* swapStack(StatTimer* s, uint64_t at_time);

    static void assertActive() { RELEASE_ASSERT(stack && !stack->isPaused(), ""); }
};
#else
struct StatTimer {
    StatTimer(int statid, bool push = true) {}
    StatTimer(int statid, uint64_t at_time) {}
    ~StatTimer() {}
    bool isPaused() const { return false; }

    void pause(uint64_t at_time) {}
    void resume(uint64_t at_time) {}

    static StatTimer* getStack() { return NULL; }
    static StatTimer* swapStack(StatTimer* s, uint64_t at_time) { return NULL; }
    static void assertActive() {}
};
#endif
}

#endif
