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

#ifndef PYSTON_UNITTESTS_UNITTESTS_H
#define PYSTON_UNITTESTS_UNITTESTS_H

#include "gtest/gtest.h"

#include "core/threading.h"
#include "codegen/entry.h"

namespace pyston {

class PystonTestEnvironment : public testing::Environment {
    void SetUp() override {
        threading::registerMainThread();

        threading::acquireGLRead();
    }

    void TearDown() override {
        threading::releaseGLRead();
    }
};

::testing::Environment* const pyston_env = ::testing::AddGlobalTestEnvironment(new PystonTestEnvironment());

} // namespace pyston

#endif
