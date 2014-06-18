// Copyright (c) 2014 Dropbox, Inc.
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

namespace pyston {

#if !DISABLE_STATS
std::vector<long>* Stats::counts;
std::unordered_map<int, std::string>* Stats::names;
StatCounter::StatCounter(const std::string& name) : id(Stats::getStatId(name)) {
}

StatPerThreadCounter::StatPerThreadCounter(const std::string& name) {
    char buf[80];
    snprintf(buf, 80, "%s_t%d", name.c_str(), threading::gettid());
    id = Stats::getStatId(buf);
}

int Stats::getStatId(const std::string& name) {
    // hacky but easy way of getting around static constructor ordering issues for now:
    static std::unordered_map<int, std::string> names;
    Stats::names = &names;
    static std::vector<long> counts;
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

void Stats::dump() {
    printf("Stats:\n");

    std::vector<std::pair<std::string, int> > pairs;
    for (const auto& p : *names) {
        pairs.push_back(make_pair(p.second, p.first));
    }

    std::sort(pairs.begin(), pairs.end());

    for (int i = 0; i < pairs.size(); i++) {
        printf("%s: %ld\n", pairs[i].first.c_str(), (*counts)[pairs[i].second]);
    }
}

#endif
}
