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

#ifndef PYSTON_CORE_THREADUTILS_H
#define PYSTON_CORE_THREADUTILS_H

#include <pthread.h>

namespace pyston {
namespace threading {

class LockedRegion {
private:
    pthread_mutex_t* mutex;

public:
    LockedRegion(pthread_mutex_t* mutex) : mutex(mutex) { pthread_mutex_lock(mutex); }
    ~LockedRegion() { pthread_mutex_unlock(mutex); }
};



template <typename T> class PerThreadSet {
public:
    pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    std::unordered_map<pthread_t, T*> map;
};

template <typename T> class PerThread {
private:
    PerThreadSet<T>* set;
    pthread_t self;

public:
    T value;

    PerThread(PerThreadSet<T>* set) : set(set), self(pthread_self()) {
        LockedRegion _lock(&set->lock);

        set->map[self] = &value;
    }

    ~PerThread() {
        LockedRegion _lock(&set->lock);

        assert(set->map.count(self) == 1);
        set->map.erase(self);
    }
};

} // namespace threading
} // namespace pyston

#endif
