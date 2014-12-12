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

#include <cmath>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "core/types.h"
#include "gc/collector.h"
#include "runtime/inline/boxing.h"
#include "runtime/types.h"
#include "runtime/util.h"

namespace pyston {

BoxedModule* posix_module;

namespace posix {

static Box* urandom(Box* _n) {
    RELEASE_ASSERT(_n->cls == int_cls, "");

    int64_t n = static_cast<BoxedInt*>(_n)->n;
    RELEASE_ASSERT(n < INT_MAX, "");

    int fd = ::open("/dev/urandom", O_RDONLY);
    RELEASE_ASSERT(fd > 0, "");

    BoxedString* r = static_cast<BoxedString*>(PyString_FromStringAndSize(NULL, n));
    RELEASE_ASSERT(r, "");
    char* buf = PyString_AsString(r);

    int total_read = 0;
    while (total_read < n) {
        int this_read = read(fd, buf, n - total_read);
        assert(this_read > 0);
        total_read += this_read;
    }
    ::close(fd);

    return r;
}

static Box* posix_getuid() {
    return boxInt(getuid());
}

} // namespace posix

void setupPosix() {
    posix_module = createModule("posix", "__builtin__");

    posix_module->giveAttr("urandom", new BoxedFunction(boxRTFunction((void*)posix::urandom, STR, 1)));
    posix_module->giveAttr("getuid", new BoxedFunction(boxRTFunction((void*)posix::posix_getuid, BOXED_INT, 0)));

    posix_module->giveAttr("error", OSError);
}
}
