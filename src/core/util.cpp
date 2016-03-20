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

#include "core/util.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>

#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FormattedStream.h"

#include "core/common.h"
#include "core/options.h"
#include "core/types.h"

namespace pyston {

#if !DISABLE_TIMERS

int Timer::level = 0;

Timer::Timer(long min_usec) : min_usec(min_usec), ended(true) {
}

Timer::Timer(const char* desc, long min_usec) : min_usec(min_usec), ended(true) {
    restart(desc);
}

void Timer::restart(const char* newdesc) {
    assert(ended);

    desc = newdesc;
    start_time = getCPUTicks();
    Timer::level++;
    ended = false;
}

void Timer::restart(const char* newdesc, long new_min_usec) {
    this->min_usec = new_min_usec;
    restart(newdesc);
}

uint64_t Timer::end(uint64_t* ended_at) {
    if (!ended) {
        uint64_t end = getCPUTicks();
        uint64_t duration = end - start_time;
        Timer::level--;
        if (VERBOSITY("time") >= 2 && desc) {
            uint64_t us = (uint64_t)(duration / Stats::estimateCPUFreq());

            if (us > min_usec) {
                for (int i = 0; i < Timer::level; i++) {
                    putchar(' ');
                }
                printf("\033[32m");
                if (us < 1000) {
                    printf("%ldus %s\n", us, desc);
                } else if (us < 1000000) {
                    printf("%.1fms %s\n", us / 1000.0, desc);
                } else {
                    printf("%.2fs %s\n", us / 1000000.0, desc);
                }
                printf("\033[0m");
                fflush(stdout);
            }
        }
        if (ended_at)
            *ended_at = end;
        ended = true;
        return duration;
    }
    return -1;
}

Timer::~Timer() {
    if (!ended) {
        uint64_t t = end();
        if (exit_callback)
            exit_callback(t);
    }
}

#endif // !DISABLE_TIMERS

void removeDirectoryIfExists(const std::string& path) {
    llvm_error_code code;

    llvm::sys::fs::file_status status;
    code = llvm::sys::fs::status(path, status);
    if (!llvm::sys::fs::exists(status))
        return;

    assert(llvm::sys::fs::is_directory(status));

    llvm::sys::fs::directory_iterator it(path, code), end;
    assert(!code);

    while (it != end) {
        code = it->status(status);
        assert(!code);

        if (llvm::sys::fs::is_directory(status)) {
            removeDirectoryIfExists(it->path());
        } else {
            if (VERBOSITY() >= 2)
                llvm::errs() << "Removing file " << it->path() << '\n';
            code = llvm::sys::fs::remove(it->path(), false);
            assert(!code);
        }

        it = it.increment(code);
        assert(!code);
    }

    if (VERBOSITY() >= 2)
        llvm::errs() << "Removing directory " << path << '\n';
    code = llvm::sys::fs::remove(path, false);
    assert(!code);
}
}
