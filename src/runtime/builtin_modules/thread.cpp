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

#include <pthread.h>
#include <stddef.h>

#include "codegen/compvars.h"
#include "core/threading.h"
#include "core/types.h"
#include "runtime/objmodel.h"
#include "runtime/types.h"

using namespace pyston::threading;

namespace pyston {

static_assert(sizeof(pthread_t) <= sizeof(BoxedInt::n), "");

BoxedModule* thread_module;

static void* thread_start(void* _args) {
    std::vector<Box*>* args = static_cast<std::vector<Box*>*>(_args);
    assert(args->size() == 2 || args->size() == 3);
    Box* target = (*args)[0];
    Box* varargs = (*args)[1];
    Box* kwargs = NULL;
    if (args->size() > 2)
        kwargs = (*args)[2];
    delete args;

    try {
        runtimeCall(target, ArgPassSpec(0, 0, true, kwargs != NULL), varargs, kwargs, NULL, NULL, NULL);
    } catch (Box* b) {
        std::string msg = formatException(b);
        printLastTraceback();
        fprintf(stderr, "%s\n", msg.c_str());
    }
    return NULL;
}

// TODO this should take kwargs, which defaults to empty
Box* startNewThread(Box* target, Box* args) {
    pthread_t thread_id;
    int code = pthread_create(&thread_id, NULL, &thread_start, new std::vector<Box*>({ target, args }));
    assert(code == 0);
    return boxInt(thread_id ^ 0x12345678901L);
}

void setupThread() {
    thread_module = createModule("thread", "__builtin__");

    thread_module->giveAttr("start_new_thread", new BoxedFunction(boxRTFunction((void*)startNewThread, BOXED_INT, 2)));
}
}
