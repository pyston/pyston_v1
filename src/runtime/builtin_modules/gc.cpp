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

#include "core/types.h"
#include "gc/collector.h"
#include "runtime/types.h"

namespace pyston {

Box* gcCollect() {
    gc::runCollection();
    return None;
}

void setupGC() {
    BoxedModule* gc_module = createModule("gc", "__builtin__");

    gc_module->giveAttr("__hex__", new BoxedFunction(boxRTFunction((void*)gcCollect, NONE, 0)));
}
}
