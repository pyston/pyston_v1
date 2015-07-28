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

#ifndef PYSTON_CORE_THREADUTILS_H
#define PYSTON_CORE_THREADUTILS_H

#include <functional>
#include <pthread.h>
#include <unordered_map>

#include "core/common.h"

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
    // NB. I tried using error-checking mutexes (PTHREAD_ERRORCHECK_MUTEX_INITIALIZER_NP) here in debug-mode but got
    // some funky errors. I think we might be deliberately locking/unlocking mutexes on different threads in some
    // circumstances. - rntz
    pthread_mutex_t mutex = PTHREAD_ADAPTIVE_MUTEX_INITIALIZER_NP;

public:
    void lock() {
        int err = pthread_mutex_lock(&mutex);
        ASSERT(!err, "pthread_mutex_lock failed, error code %d", err);
    }
    void unlock() {
        int err = pthread_mutex_unlock(&mutex);
        ASSERT(!err, "pthread_mutex_unlock failed, error code %d", err);
    }

    PthreadFastMutex* asRead() { return this; }
    PthreadFastMutex* asWrite() { return this; }
};

class PthreadMutex {
private:
    // Ditto comment in PthreadFastMutex re error-checking mutexes. - rntz
    pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

public:
    void lock() {
        int err = pthread_mutex_lock(&mutex);
        ASSERT(!err, "pthread_mutex_lock failed, error code %d", err);
    }
    void unlock() {
        int err = pthread_mutex_unlock(&mutex);
        ASSERT(!err, "pthread_mutex_unlock failed, error code %d", err);
    }

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


namespace impl {
// From http://stackoverflow.com/questions/7858817/unpacking-a-tuple-to-call-a-matching-function-pointer
template <int...> struct seq {};

template <int N, int... S> struct gens : gens<N - 1, N - 1, S...> {};

template <int... S> struct gens<0, S...> { typedef seq<S...> type; };
}

class PerThreadSetBase {
private:
    static std::unordered_set<PerThreadSetBase*> all_instances;

protected:
    PerThreadSetBase() { all_instances.insert(this); }

    virtual ~PerThreadSetBase() {
        assert(all_instances.count(this));
        all_instances.erase(this);
    }

    virtual void onFork() = 0;

public:
    static void runAllForkHandlers() {
        for (auto inst : all_instances)
            inst->onFork();
    }
};

template <typename T, typename... CtorArgs> class PerThreadSet : public PerThreadSetBase {
private:
    pthread_key_t pthread_key;
    PthreadFastMutex lock;

    struct Storage {
        PerThreadSet<T, CtorArgs...>* self;
#ifndef NDEBUG
        pthread_t my_tid;
#endif
        T val;
    };

    std::unordered_map<pthread_t, Storage*> map;
    std::tuple<CtorArgs...> ctor_args;
#ifndef NDEBUG
    int map_elts = 0;
#endif

    static void dtor(void* val) {
        Storage* s = static_cast<Storage*>(val);
        assert(s);

        auto* self = s->self;

        LOCK_REGION(&self->lock);

        ASSERT(self->map.size() == self->map_elts, "%ld %d", self->map.size(), self->map_elts);
        assert(s->my_tid == pthread_self());

        assert(self->map.count(pthread_self()));
        self->map.erase(pthread_self());
#ifndef NDEBUG
        self->map_elts--;
#endif

        delete s;
    }

    template <int... S> Storage* make(impl::seq<S...>) {
        return new Storage{.self = this,
#ifndef NDEBUG
                           .my_tid = pthread_self(),
#endif
                           .val = T(std::get<S>(ctor_args)...) };
    }

protected:
    void onFork() override {
        pthread_t surviving_ptid = pthread_self();
        for (auto it = this->map.begin(), end = this->map.end(); it != end;) {
            if (it->first != surviving_ptid) {
                delete it->second;
                it = this->map.erase(it);
#ifndef NDEBUG
                this->map_elts--;
#endif
            } else
                ++it;
        }
    }

public:
    PerThreadSet(CtorArgs... ctor_args) : ctor_args(std::forward<CtorArgs>(ctor_args)...) {
        int code = pthread_key_create(&pthread_key, &dtor);
    }

    void forEachValue(std::function<void(T*)> f) {
        LOCK_REGION(&lock);

        for (auto& p : map) {
            f(&p.second->val);
        }
    }

    template <typename... Arguments> void forEachValue(std::function<void(T*, Arguments...)> f, Arguments... args) {
        LOCK_REGION(&lock);

        for (auto& p : map) {
            f(&p.second->val, std::forward<Arguments>(args)...);
        }
    }

    T* get() {
        // Is there even much benefit to using pthread_getspecific here, as opposed to looking
        // it up in the map?  I suppose it avoids locking
        Storage* s = static_cast<Storage*>(pthread_getspecific(pthread_key));
        if (!s) {
            s = make(typename impl::gens<sizeof...(CtorArgs)>::type());

            LOCK_REGION(&lock);

#ifndef NDEBUG
            assert(map.size() == map_elts);
            map_elts++;
#endif

            int code = pthread_setspecific(pthread_key, s);
            assert(code == 0);

            map[pthread_self()] = s;
        }
        return &s->val;
    }
};

} // namespace threading
} // namespace pyston

#endif
