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

#include <cmath>
#include <ctime>
#include <err.h>
#include <sys/time.h>

#include "core/threading.h"
#include "core/types.h"
#include "runtime/objmodel.h"
#include "runtime/types.h"

namespace pyston {

BoxedModule* time_module;

/* Exposed in timefuncs.h. */
extern "C" time_t _PyTime_DoubleToTimet(double x) noexcept {
    time_t result;
    double diff;

    result = (time_t)x;
    /* How much info did we lose?  time_t may be an integral or
     * floating type, and we don't know which.  If it's integral,
     * we don't know whether C truncates, rounds, returns the floor,
     * etc.  If we lost a second or more, the C rounding is
     * unreasonable, or the input just doesn't fit in a time_t;
     * call it an error regardless.  Note that the original cast to
     * time_t can cause a C error too, but nothing we can do to
     * worm around that.
     */
    diff = x - (double)result;
    if (diff <= -1.0 || diff >= 1.0) {
        PyErr_SetString(PyExc_ValueError, "timestamp out of range for platform time_t");
        result = (time_t)-1;
    }
    return result;
}

Box* timeTime() {
    struct timeval now;
    gettimeofday(&now, NULL);
    double t = now.tv_sec + .000001 * now.tv_usec;
    return boxFloat(t);
}

Box* timeSleep(Box* arg) {
    double secs;
    if (isSubclass(arg->cls, int_cls))
        secs = static_cast<BoxedInt*>(arg)->n;
    else if (arg->cls == float_cls)
        secs = static_cast<BoxedFloat*>(arg)->d;
    else {
        raiseExcHelper(TypeError, "a float is required");
    }

    double fullsecs;
    double nanosecs = modf(secs, &fullsecs);

    struct timespec req;
    req.tv_sec = (int)(fullsecs + 0.01);
    req.tv_nsec = (int)(nanosecs * 1000000000);

    {
        threading::GLAllowThreadsReadRegion _allow_threads;
        int code = nanosleep(&req, NULL);

        if (code)
            err(1, NULL);
    }

    return None;
}

void setupTime() {
    time_module = createModule("time", "__builtin__");

    time_module->giveAttr("time", new BoxedFunction(boxRTFunction((void*)timeTime, BOXED_FLOAT, 0)));
    time_module->giveAttr("sleep", new BoxedFunction(boxRTFunction((void*)timeSleep, NONE, 1)));
}
}
