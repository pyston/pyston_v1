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

static Box* gcCollect() {
    gc::runCollection();

    // I think it's natural that the user would expect the finalizers to get run here if we're forcing
    // a GC pass. It should be safe to do, and makes testing easier also.
    gc::callPendingDestructionLogic();

    return None;
}

static Box* isEnabled() {
    return boxBool(gc::gcIsEnabled());
}

static Box* disable() {
    gc::disableGC();
    return None;
}

static Box* enable() {
    gc::enableGC();
    return None;
}

void setupGC() {
    BoxedModule* gc_module = createModule("gc");

    gc_module->giveAttr("collect",
                        new BoxedBuiltinFunctionOrMethod(boxRTFunction((void*)gcCollect, NONE, 0), "collect"));
    gc_module->giveAttr("isenabled",
                        new BoxedBuiltinFunctionOrMethod(boxRTFunction((void*)isEnabled, BOXED_BOOL, 0), "isenabled"));
    gc_module->giveAttr("disable", new BoxedBuiltinFunctionOrMethod(boxRTFunction((void*)disable, NONE, 0), "disable"));
    gc_module->giveAttr("enable", new BoxedBuiltinFunctionOrMethod(boxRTFunction((void*)enable, NONE, 0), "enable"));
}
}
