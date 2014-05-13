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

#include <ctime>
#include <sys/time.h>

#include "core/types.h"

#include "runtime/gc_runtime.h"
#include "runtime/types.h"

namespace pyston {

BoxedModule* time_module;

Box* timeTime() {
    struct timeval now;
    gettimeofday(&now, NULL);
    double t = now.tv_sec + .000001 * now.tv_usec;
    return boxFloat(t);
}

void setupTime() {
    time_module = createModule("time", "__builtin__");

    time_module->giveAttr("time", new BoxedFunction(boxRTFunction((void*)timeTime, NULL, 0, false)));
}
}
