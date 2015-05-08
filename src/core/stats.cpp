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
extern "C" const char* getStatTimerNameById(int id) {
    return Stats::getStatName(id).c_str();
}

extern "C" int getStatTimerId() {
    return StatTimer::getStack()->getId();
}

extern "C" const char* getStatTimerName() {
    return getStatTimerNameById(getStatTimerId());
}

__thread StatTimer* StatTimer::stack;
StatTimer::StatTimer(int statid, bool push) {
    uint64_t at_time = getCPUTicks();
    _start_time = 0;
    _statid = statid;

    if (!push) {
        _prev = NULL;
        return;
    }

    _prev = stack;
    stack = this;
    if (_prev) {
        _prev->pause(at_time);
    }
    resume(at_time);
}

StatTimer::StatTimer(int statid, uint64_t at_time) {
    _start_time = 0;
    _statid = statid;
    _prev = stack;
    stack = this;
    if (_prev) {
        _prev->pause(at_time);
    }
    resume(at_time);
}

StatTimer::~StatTimer() {
    assert(stack == this);

    uint64_t at_time;
    if (!isPaused()) {
        at_time = getCPUTicks();
        pause(at_time);
    } else {
        // fprintf (stderr, "WARNING: timer was paused.\n");
        at_time = _last_pause_time;
    }

    stack = _prev;
    if (stack) {
        stack->resume(at_time);
    }
}

void StatTimer::pause(uint64_t at_time) {
    assert(!isPaused());
    assert(at_time > _start_time);

    uint64_t _duration = at_time - _start_time;
    Stats::log(_statid, _duration);

    _start_time = 0;
    _last_pause_time = at_time;
    // fprintf (stderr, "paused  %d at %lu\n", _statid, at_time);
}

void StatTimer::resume(uint64_t at_time) {
    assert(isPaused());
    _start_time = at_time;
    // fprintf (stderr, "resumed %d at %lu\n", _statid, at_time);
}

StatTimer* StatTimer::swapStack(StatTimer* s, uint64_t at_time) {
    StatTimer* prev_stack = stack;
    if (stack) {
        stack->pause(at_time);
    }
    stack = s;
    if (stack) {
        stack->resume(at_time);
    }
    return prev_stack;
}
#endif

std::vector<uint64_t>* Stats::counts;
std::unordered_map<int, std::string>* Stats::names;
bool Stats::enabled;

timespec Stats::start_ts;
uint64_t Stats::start_tick;

StatCounter::StatCounter(const std::string& name) : id(Stats::getStatId(name)) {
}

StatPerThreadCounter::StatPerThreadCounter(const std::string& name) {
    char buf[80];
    snprintf(buf, 80, "%s_t%ld", name.c_str(), pthread_self());
    id = Stats::getStatId(buf);
}

int Stats::getStatId(const std::string& name) {
    // hacky but easy way of getting around static constructor ordering issues for now:
    static std::unordered_map<int, std::string> names;
    Stats::names = &names;
    static std::vector<uint64_t> counts;
    Stats::counts = &counts;
    static std::unordered_map<std::string, int> made;

    if (made.count(name))
        return made[name];

    int rtn = names.size();
    names[rtn] = name;
    made[name] = rtn;
    counts.push_back(0);
    return rtn;
}

std::string Stats::getStatName(int id) {
    return (*names)[id];
}

void Stats::startEstimatingCPUFreq() {
    if (!Stats::enabled)
        return;

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

    std::vector<std::pair<std::string, int>> pairs;
    for (const auto& p : *names) {
        pairs.push_back(make_pair(p.second, p.first));
    }

    std::sort(pairs.begin(), pairs.end());

    uint64_t ticks_in_main = 0;
    uint64_t accumulated_stat_timer_ticks = 0;
    for (int i = 0; i < pairs.size(); i++) {
        if (includeZeros || (*counts)[pairs[i].second] > 0) {
            if (startswith(pairs[i].first, "us_") || startswith(pairs[i].first, "_init_us_")) {
                fprintf(stderr, "%s: %lu\n", pairs[i].first.c_str(),
                        (uint64_t)((*counts)[pairs[i].second] / cycles_per_us));

            } else
                fprintf(stderr, "%s: %lu\n", pairs[i].first.c_str(), (*counts)[pairs[i].second]);

            if (startswith(pairs[i].first, "us_timer_"))
                accumulated_stat_timer_ticks += (*counts)[pairs[i].second];

            if (pairs[i].first == "ticks_in_main")
                ticks_in_main = (*counts)[pairs[i].second];
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
    int orig_names = names->size();
    for (int orig_id = 0; orig_id < orig_names; orig_id++) {
        int init_id = getStatId("_init_" + (*names)[orig_id]);
        log(init_id, (*counts)[orig_id]);
    }
};

#endif
}
