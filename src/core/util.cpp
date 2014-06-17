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

#include "core/util.h"

#include <cstdio>
#include <cstdlib>

#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FormattedStream.h"

#include "core/common.h"
#include "core/options.h"

namespace pyston {

int Timer::level = 0;

Timer::Timer(const char* desc) : min_usec(-1), ended(true) {
    restart(desc);
}
Timer::Timer(const char* desc, long min_usec) : min_usec(min_usec), ended(true) {
    restart(desc);
}

void Timer::restart(const char* newdesc) {
    assert(ended);

    desc = newdesc;
    gettimeofday(&start_time, NULL);
    Timer::level++;
    ended = false;
}

void Timer::restart(const char* newdesc, long new_min_usec) {
    this->min_usec = new_min_usec;
    restart(newdesc);
}

long Timer::end() {
    if (!ended) {
        timeval end;
        gettimeofday(&end, NULL);
        long us = 1000000L * (end.tv_sec - start_time.tv_sec) + (end.tv_usec - start_time.tv_usec);

        Timer::level--;
        if (VERBOSITY("time") >= 1) {
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
        ended = true;
        return us;
    }
    return -1;
}

Timer::~Timer() {
    end();
}

bool startswith(const std::string& s, const std::string& pattern) {
    if (s.size() == 0)
        return pattern.size() == 0;
    return s.compare(0, pattern.size(), pattern) == 0;
}

void removeDirectoryIfExists(const std::string& path) {
    llvm::error_code code;

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
            if (VERBOSITY())
                llvm::errs() << "Removing file " << it->path() << '\n';
            code = llvm::sys::fs::remove(it->path(), false);
            assert(!code);
        }

        it = it.increment(code);
        assert(!code);
    }

    if (VERBOSITY())
        llvm::errs() << "Removing directory " << path << '\n';
    code = llvm::sys::fs::remove(path, false);
    assert(!code);
}
}
