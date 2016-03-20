// Copyright (c) 2014-2016 Dropbox, Inc.
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

// This file is a hack for debugging deadlocks. It makes pthread_mutex_lock() complain if it takes more than given time
// (TIMEOUT_S) to grab a lock. Perhaps it will be useful in future.

#if 0 // set to 1 to enable

#include <errno.h>
#include <pthread.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "core/common.h"

#define TIMEOUT_S 2

extern "C" int pthread_mutex_lock(pthread_mutex_t* lock) {
    struct timespec timeout;
    memset(&timeout, 0, sizeof timeout);
    timeout.tv_sec = TIMEOUT_S;

    pid_t tid = syscall(SYS_gettid);
    RELEASE_ASSERT(tid > 1, "negative or invalid TID");

    time_t started = time(NULL);
    RELEASE_ASSERT(started != (time_t)-1, "could not get time()");

    int err;
    for (;;) {
        err = pthread_mutex_timedlock(lock, &timeout);
        if (err != ETIMEDOUT)
            break;
        time_t now = time(NULL);
        RELEASE_ASSERT(now != (time_t)-1, "could not get time()");
        if (now - started >= TIMEOUT_S) {
            printf("%d: mutex %p TIMED OUT\n", tid, (void*)lock);
            started = now;
        }
    }
    RELEASE_ASSERT(!err, "could not lock mutex, error %d", err);
    return err;
}

#endif
