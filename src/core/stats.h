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

// Set this to 1 to disable all stats-related operations.  Shouldn't usually be necessary.
#define DISABLE_STATS 0

// Enable certain expensive stat collections:
#define STAT_ALLOCATIONS (0 && !DISABLE_STATS)
#define STAT_ALLOCATION_TYPES (0 && !DISABLE_STATS)
#define STAT_CALLATTR_DESCR_ABORTS (0 && !DISABLE_STATS)
#define STAT_EXCEPTIONS (0 && !DISABLE_STATS)
#define STAT_EXCEPTIONS_LOCATION (0 && STAT_EXCEPTIONS)
#define STAT_ICS (0 && !DISABLE_STATS)
#define STAT_ICS_LOCATION (0 && STAT_ICS)
#define STAT_TIMERS (0 && !DISABLE_STATS)
#define EXPENSIVE_STAT_TIMERS (0 && STAT_TIMERS)

#if STAT_TIMERS
#define STAT_TIMER(id, name, avoidability)                                                                             \
    static uint64_t* _stcounter##id = Stats::getStatCounter(name);                                                     \
    ScopedStatTimer _st##id(_stcounter##id, avoidability)
#define UNAVOIDABLE_STAT_TIMER(id, name)                                                                               \
    static uint64_t* _stcounter##id = Stats::getStatCounter(name);                                                     \
    ScopedStatTimer _st##id(_stcounter##id, 0, true)
#else
#define STAT_TIMER(id, name, avoidability)
#define UNAVOIDABLE_STAT_TIMER(id, name)
#endif

#define STAT_TIMER_NAME(id) _st##id

#if !DISABLE_STATS
// The class that stores and manages stats collection.  For normal stats collections purposes,
// you shouldn't have to use this class, and will usually want to use StatCounter instead.
struct Stats {
private:
    static std::unordered_map<uint64_t*, std::string>* names;
    static bool enabled;

    static timespec start_ts;
    static uint64_t start_tick;

public:
    static void startEstimatingCPUFreq();
    static double estimateCPUFreq();

    static uint64_t* getStatCounter(const std::string& name);

    static void setEnabled(bool enabled) { Stats::enabled = enabled; }
    static void log(uint64_t* counter, uint64_t count = 1) { *counter += count; }

    static void clear();
    static void dump(bool includeZeros = true);
    static void endOfInit();
};

// A helper class for efficient stats collections.  Typical usage:
//
//     static StatCounter my_stat_counter("my_informative_stat_name");
//     void myFunction() {
//         my_stat_counter.log();
//     }
//
// The current convention for stat names is underscore_case, such as `num_cxa_throw`,
// though at some point we'd like to move to a period-delimited convention.
// (Run `./pyston -s` to see the list of current stats we log.)
// For single stats, usually `num_foo` is a decent name.  If there are many stats in a
// single category, you can drop the `num_`.
// If a stat name is a prefix of another, the event it is counting should be a superset.
// For instance, `ic_rewrites` counts a superset of the events that `ic_rewrites_aborted`
// counts, which itself is a superset of the events that `ic_rewrites_aborted_assemblyfail`
// counts.
struct StatCounter {
private:
    uint64_t* counter;

public:
    StatCounter(const std::string& name);

    void log(uint64_t count = 1) { *counter += count; }
};

// Similar to StatCounter, but should be allocated as:
//
//     static thread_local StatPerThreadCounter my_stat_counter("cool_stat_name");
//
// This will automatically add the thread id to the stat name.
struct StatPerThreadCounter {
private:
    uint64_t* counter = 0;

public:
    StatPerThreadCounter(const std::string& name);

    void log(uint64_t count = 1) { *counter += count; }
};

#else
struct Stats {
    static void startEstimatingCPUFreq() {}
    static double estimateCPUFreq() { return 0; }
    static void setEnabled(bool enabled) {}
    static void dump(bool includeZeros = true) { printf("(Stats disabled)\n"); }
    static void clear() {}
    static void log(uint64_t* counter, int count = 1) {}
    static uint64_t* getStatCounter(const std::string& name) { return nullptr; }
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

// StatTimers are for a specific type of profiling investigation.  Until we make this more usable,
// there probably shouldn't be more changes or uses of this class.
class StatTimer {
private:
    static __thread StatTimer* stack;

    // the start time of the current active segment (0 == paused)
    uint64_t _start_time;

    StatTimer* _prev;

    uint64_t* _statcounter;
    int avoidability;
    bool reset_avoidability;

    static __thread uint64_t* counter_override;

public:
    StatTimer(uint64_t* counter, int avoidability, bool reset_avoidability = false)
        : _statcounter(counter), avoidability(avoidability), reset_avoidability(reset_avoidability) {}

    static void overrideCounter(uint64_t* new_counter) {
        assert(!counter_override);
        counter_override = new_counter;
    }

    static void finishOverride() {
        assert(counter_override);
        counter_override = NULL;
    }

    void pushNonTopLevel() {
#ifndef NDEBUG
        _start_time = 0;
#endif

        assert(stack);
        _prev = stack;

        if (reset_avoidability || avoidability >= _prev->avoidability) {
            uint64_t at_time = getCPUTicks();

            stack = this;
            _prev->pause(at_time);
            resume(at_time);
        }
    }

    void popNonTopLevel() {
        if (reset_avoidability || avoidability >= _prev->avoidability) {
            assert(stack == this);

            uint64_t at_time;
            assert(!isPaused());
            at_time = getCPUTicks();
            pause(at_time);

            assert(_prev);
            stack = _prev;
            stack->resume(at_time);
        }
    }

    void pushTopLevel(uint64_t at_time) {
#ifndef NDEBUG
        _start_time = 0;
#endif
        assert(!stack);
        _prev = stack;
        stack = this;
        resume(at_time);
    }

    void popTopLevel(uint64_t at_time) {
        assert(!_prev);
        stack = _prev;
        pause(at_time);
    }

private:
    void pause(uint64_t at_time) {
        assert(!isPaused());
        assert(at_time > _start_time);

        uint64_t _duration = at_time - _start_time;
        if (counter_override)
            Stats::log(counter_override, _duration);
        else
            Stats::log(_statcounter, _duration);

        _start_time = 0;
    }

    void resume(uint64_t at_time) {
        assert(isPaused());
        _start_time = at_time;
    }

public:
#ifndef NDEBUG
    bool isPaused() const { return _start_time == 0; }
#endif

    // Creates a new stattimer stack from an unstarted top-level timer
    static StatTimer* createStack(StatTimer& timer);
    static StatTimer* swapStack(StatTimer* s);

    static uint64_t* getCurrentCounter();

    static void assertActive() { ASSERT(stack && !stack->isPaused(), ""); }
};

// Helper class around a StatTimer
class ScopedStatTimer {
private:
    StatTimer timer;

public:
    ScopedStatTimer(uint64_t* counter, int avoidability, bool reset_avoidability = false)
        : timer(counter, avoidability, reset_avoidability) {
        timer.pushNonTopLevel();
    }
    ~ScopedStatTimer() { timer.popNonTopLevel(); }
};

#else
struct StatTimer {
    StatTimer(uint64_t*) {}
    ~StatTimer() {}
    bool isPaused() const { return false; }

    void pause(uint64_t at_time) {}
    void resume(uint64_t at_time) {}

    static StatTimer* createStack(StatTimer& timer);
    static StatTimer* swapStack(StatTimer* s);
    static void assertActive() {}
};
#endif
}

#endif
