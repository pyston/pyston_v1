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

#include "core/stats.h"

#include <algorithm>

#include "core/thread_utils.h"
#include "gc/heap.h"

namespace pyston {

#if !DISABLE_STATS
#if STAT_TIMERS

__thread StatTimer* StatTimer::stack;
__thread uint64_t* StatTimer::counter_override;

StatTimer* StatTimer::swapStack(StatTimer* s) {
    uint64_t at_time = getCPUTicks();

    assert(stack);
    assert(s);
    StatTimer* prev_stack = stack;
    stack->pause(at_time);
    stack = s;
    stack->resume(at_time);
    assert(prev_stack);
    return prev_stack;
}

StatTimer* StatTimer::createStack(StatTimer& timer) {
    uint64_t at_time = getCPUTicks();

    assert(stack);
    StatTimer* prev_stack = stack;
    stack->pause(at_time);
    stack = NULL;

    timer.pushTopLevel(at_time);
    return prev_stack;
}

uint64_t* StatTimer::getCurrentCounter() {
    if (counter_override)
        return counter_override;
    if (stack)
        return stack->_statcounter;
    return NULL;
}
#endif

std::unordered_map<uint64_t*, std::string>* Stats::names;
bool Stats::enabled;

timespec Stats::start_ts;
uint64_t Stats::start_tick;

StatCounter::StatCounter(const std::string& name) : counter(Stats::getStatCounter(name)) {
}

StatPerThreadCounter::StatPerThreadCounter(const std::string& name) {
    char buf[80];
    snprintf(buf, 80, "%s_t%ld", name.c_str(), pthread_self());
    counter = Stats::getStatCounter(buf);
}

static std::vector<uint64_t*>* counts;
uint64_t* Stats::getStatCounter(const std::string& name) {
    // hacky but easy way of getting around static constructor ordering issues for now:
    static std::unordered_map<uint64_t*, std::string> names;
    Stats::names = &names;
    static std::unordered_map<std::string, uint64_t*> made;
    // TODO: can do better than doing a malloc per counter:
    static std::vector<uint64_t*> counts;
    pyston::counts = &counts;

    if (made.count(name))
        return made[name];

    uint64_t* rtn = new uint64_t(0);
    names[rtn] = name;
    made[name] = rtn;
    counts.push_back(rtn);
    return rtn;
}

void Stats::clear() {
    assert(counts);
    for (auto p : *counts) {
        *p = 0;
    }
}

void Stats::startEstimatingCPUFreq() {
    clock_gettime(CLOCK_REALTIME, &Stats::start_ts);
    Stats::start_tick = getCPUTicks();
}

// returns our estimate of the MHz of the cpu.  MHz is handy because we're mostly interested in microsoecond-resolution
// timing.
double Stats::estimateCPUFreq() {
    timespec dump_ts;
    clock_gettime(CLOCK_REALTIME, &dump_ts);
    uint64_t end_tick = getCPUTicks();

    uint64_t wall_clock_ns = (dump_ts.tv_sec - start_ts.tv_sec) * 1000000000 + (dump_ts.tv_nsec - start_ts.tv_nsec);
    return (double)(end_tick - Stats::start_tick) * 1000 / wall_clock_ns;
}

void Stats::dump(bool includeZeros) {
    if (!Stats::enabled)
        return;

    double cycles_per_us = Stats::estimateCPUFreq();
    fprintf(stderr, "Stats:\n");
    fprintf(stderr, "estimated_cpu_mhz: %5.5f\n", cycles_per_us);

    gc::dumpHeapStatistics(0);

    fprintf(stderr, "Counters:\n");

    std::vector<std::pair<std::string, uint64_t*>> pairs;
    for (const auto& p : *names) {
        pairs.push_back(make_pair(p.second, p.first));
    }

    std::sort(pairs.begin(), pairs.end());

    uint64_t ticks_in_main = 0;
    uint64_t accumulated_stat_timer_ticks = 0;
    for (int i = 0; i < pairs.size(); i++) {
        uint64_t count = *pairs[i].second;
        if (includeZeros || count > 0) {
            if (startswith(pairs[i].first, "us_") || startswith(pairs[i].first, "_init_us_")) {
                fprintf(stderr, "%s: %lu\n", pairs[i].first.c_str(), (uint64_t)(count / cycles_per_us));

            } else
                fprintf(stderr, "%s: %lu\n", pairs[i].first.c_str(), count);

            if (startswith(pairs[i].first, "us_timer_"))
                accumulated_stat_timer_ticks += count;

            if (pairs[i].first == "ticks_in_main")
                ticks_in_main = count;
        }
    }

    if (includeZeros || accumulated_stat_timer_ticks > 0)
        fprintf(stderr, "ticks_all_timers: %lu\n", accumulated_stat_timer_ticks);

#if 0
    // I want to enable this, but am leaving it disabled for the time
    // being because it causes test failures due to:
    //
    // 1) some tests exit from main from inside catch blocks, without
    //    going through the logic to stop the timers.
    // 2) some tests create multiple threads which causes problems
    //    with our non-per thread stat timers.

    if (ticks_in_main && ticks_in_main != accumulated_stat_timer_ticks) {
        fprintf(stderr, "WARNING: accumulated stat timer ticks != ticks in main - don't trust timer output.");
    }
#endif
    fprintf(stderr, "(End of stats)\n");
}

void Stats::endOfInit() {
    std::unordered_map<uint64_t*, std::string> names_copy(names->begin(), names->end());
    for (const auto& p : names_copy) {
        uint64_t* init_id = getStatCounter("_init_" + p.second);
        log(init_id, *p.first);
    }
};

#endif
}
