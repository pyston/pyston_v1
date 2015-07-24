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

#ifndef PYSTON_CORE_CONTIGUOUSMAP_H
#define PYSTON_CORE_CONTIGUOUSMAP_H

#include <llvm/ADT/DenseMap.h>
#include <utility>
#include <vector>

namespace pyston {

template <class TKey, class TVal, class TMap = llvm::DenseMap<TKey, int>, class TVec = std::vector<TVal>>
class ContiguousMap {
    typedef TMap map_type;
    typedef TVec vec_type;

    map_type map;
    vec_type vec;

    std::vector<int> free_list;

public:
    typedef typename map_type::iterator iterator;
    typedef typename map_type::const_iterator const_iterator;
    typedef typename std::vector<TVal>::size_type size_type;

    iterator find(TKey key) { return map.find(key); }

    iterator begin() noexcept { return map.begin(); }
    iterator end() noexcept { return map.end(); }

    const_iterator begin() const noexcept { return map.begin(); }
    const_iterator end() const noexcept { return map.end(); }

    void erase(const_iterator position) {
        int idx = map[position->first];
        free_list.push_back(idx);
        vec[idx] = TVal();
        map.erase(position->first);
    }

    size_type erase(const TKey& key) {
        auto it = map.find(key);
        if (it == end())
            return 0;
        int idx = it->second;
        free_list.push_back(idx);
        vec[idx] = TVal();
        map.erase(it);
        return 1;
    }

    size_type count(const TKey& key) const { return map.count(key); }

    TVal& operator[](const TKey& key) {
        auto it = map.find(key);
        if (it == map.end()) {
            int idx;
            if (free_list.size() > 0) {
                idx = free_list.back();
                free_list.pop_back();
            } else {
                idx = vec.size();
                vec.push_back(TVal());
            }
            map[key] = idx;
            return vec[idx];
        } else {
            return vec[it->second];
        }
    }

    TVal& operator[](TKey&& key) {
        auto it = map.find(key);
        if (it == map.end()) {
            int idx;
            if (free_list.size() > 0) {
                idx = free_list.back();
                free_list.pop_back();
            } else {
                idx = vec.size();
                vec.push_back(TVal());
            }
            map[key] = idx;
            return vec[idx];
        } else {
            return vec[it->second];
        }
    }

    TVal getMapped(int idx) const { return vec[idx]; }

    size_type size() const { return map.size(); }
    bool empty() const { return map.empty(); }
    const vec_type& vector() { return vec; }
};
}
#endif
