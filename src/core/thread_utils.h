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
#include <unordered_map>

namespace pyston {
namespace threading {

template <typename T> class _LockedRegion {
private:
    T* const mutex;

public:
    _LockedRegion(T* mutex) : mutex(mutex) { mutex->lock(); }
    ~_LockedRegion() { mutex->unlock(); }
};

template <typename T> _LockedRegion<T> _makeLockedRegion(T* mutex) {
    return _LockedRegion<T>(mutex);
}
template <typename T> _LockedRegion<T> _makeLockedRegion(T& mutex) {
    return _LockedRegion<T>(&mutex);
}
#define LOCK_REGION(lock) auto CAT(_lock_, __LINE__) = pyston::threading::_makeLockedRegion(lock)

class NopLock {
public:
    void lock() {}
    void unlock() {}

    NopLock* asRead() { return this; }
    NopLock* asWrite() { return this; }
};

class PthreadFastMutex {
private:
    pthread_mutex_t mutex = PTHREAD_ADAPTIVE_MUTEX_INITIALIZER_NP;

public:
    void lock() { pthread_mutex_lock(&mutex); }
    void unlock() { pthread_mutex_unlock(&mutex); }

    PthreadFastMutex* asRead() { return this; }
    PthreadFastMutex* asWrite() { return this; }
};

class PthreadMutex {
private:
    pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

public:
    void lock() { pthread_mutex_lock(&mutex); }
    void unlock() { pthread_mutex_unlock(&mutex); }

    PthreadMutex* asRead() { return this; }
    PthreadMutex* asWrite() { return this; }
};

class PthreadRWLock {
private:
    pthread_rwlock_t rwlock = PTHREAD_RWLOCK_INITIALIZER;

public:
    class PthreadRWLockRead {
    private:
        pthread_rwlock_t rwlock;
        PthreadRWLockRead() = delete;

    public:
        void lock() { pthread_rwlock_rdlock(&rwlock); }
        void unlock() { pthread_rwlock_unlock(&rwlock); }
    };

    class PthreadRWLockWrite {
    private:
        pthread_rwlock_t rwlock;
        PthreadRWLockWrite() = delete;

    public:
        void lock() { pthread_rwlock_wrlock(&rwlock); }
        void unlock() { pthread_rwlock_unlock(&rwlock); }
    };

    PthreadRWLockRead* asRead() { return reinterpret_cast<PthreadRWLockRead*>(this); }

    PthreadRWLockWrite* asWrite() { return reinterpret_cast<PthreadRWLockWrite*>(this); }
};

class PthreadSpinLock {
private:
    pthread_spinlock_t spinlock;

public:
    PthreadSpinLock() { pthread_spin_init(&spinlock, false); }

    void lock() { pthread_spin_lock(&spinlock); }
    void unlock() { pthread_spin_unlock(&spinlock); }

    PthreadSpinLock* asRead() { return this; }
    PthreadSpinLock* asWrite() { return this; }
};


template <typename T> class PerThreadSet {
public:
    PthreadFastMutex lock;
    std::unordered_map<pthread_t, T*> map;
};

template <typename T> class PerThread {
private:
    PerThreadSet<T>* set;
    pthread_t self;

public:
    T value;

    PerThread(PerThreadSet<T>* set) : set(set), self(pthread_self()) {
        LOCK_REGION(&set->lock);

        set->map[self] = &value;
    }

    ~PerThread() {
        LOCK_REGION(&set->lock);

        assert(set->map.count(self) == 1);
        set->map.erase(self);
    }
};

} // namespace threading
} // namespace pyston

#endif
