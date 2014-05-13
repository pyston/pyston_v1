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

#ifndef PYSTON_ANALYSIS_FPC_H
#define PYSTON_ANALYSIS_FPC_H

#include "core/common.h"
#include "core/options.h"

#include "core/cfg.h"

namespace pyston {

template <typename T> class BBAnalyzer {
public:
    typedef std::unordered_map<std::string, T> Map;
    typedef std::unordered_map<CFGBlock*, Map> AllMap;

    virtual ~BBAnalyzer() {}

    virtual T merge(T from, T into) const = 0;
    virtual T mergeBlank(T into) const = 0;
    virtual void processBB(Map& starting, CFGBlock* block) const = 0;
};

template <typename T>
typename BBAnalyzer<T>::AllMap computeFixedPoint(CFG* cfg, const BBAnalyzer<T>& analyzer, bool reverse) {
    assert(!reverse);

    typedef typename BBAnalyzer<T>::Map Map;
    typedef typename BBAnalyzer<T>::AllMap AllMap;
    AllMap states;

    std::vector<CFGBlock*> q;

    states.insert(make_pair(cfg->getStartingBlock(), Map()));
    q.push_back(cfg->getStartingBlock());

    while (q.size()) {
        CFGBlock* block = q.back();
        q.pop_back();

        Map initial = states[block];
        if (VERBOSITY("analysis") >= 2)
            printf("fpc on block %d - %ld entries\n", block->idx, initial.size());

        Map ending = Map(initial);
        analyzer.processBB(ending, block);

        for (int i = 0; i < block->successors.size(); i++) {
            CFGBlock* next_block = block->successors[i];
            bool changed = false;
            bool initial = false;
            if (states.count(next_block) == 0) {
                changed = true;
                initial = true;
            }

            Map& next = states[next_block];
            for (const auto& p : ending) {
                if (next.count(p.first) == 0) {
                    changed = true;
                    if (initial) {
                        next[p.first] = p.second;
                    } else {
                        next[p.first] = analyzer.mergeBlank(p.second);
                    }
                } else {
                    T& next_elt = next[p.first];

                    T new_elt = analyzer.merge(p.second, next_elt);
                    if (next_elt != new_elt) {
                        next_elt = new_elt;
                        changed = true;
                    }
                }
            }

            for (const auto& p : ending) {
                if (ending.count(p.first))
                    continue;

                T next_elt = analyzer.mergeBlank(p.second);
                if (next_elt != p.second) {
                    next[p.first] = next_elt;
                    changed = true;
                }
            }

            if (changed)
                q.push_back(next_block);
        }

        states.erase(block);
        states.insert(make_pair(block, ending));
    }

    return states;
}
}

#endif
