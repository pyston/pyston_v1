//===- llvm/ADT/SmallVector.cpp - 'Normally small' vectors ----------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements the SmallVector class.
//
//===----------------------------------------------------------------------===//

// Pyston changes Copyright (c) 2014-2015 Dropbox, Inc.
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

#include "core/SmallVector.h"

using namespace pyston;

/// grow_pod - This is an implementation of the grow() method which only works
/// on POD-like datatypes and is out of line to reduce code duplication.
void SmallVectorBase::grow_pod(void* FirstEl, size_t MinSizeInBytes, size_t TSize) {
    size_t CurSizeBytes = size_in_bytes();
    size_t NewCapacityInBytes = 2 * capacity_in_bytes() + TSize; // Always grow.
    if (NewCapacityInBytes < MinSizeInBytes)
        NewCapacityInBytes = MinSizeInBytes;

    void* NewElts;
    if (BeginX == FirstEl) {
        NewElts = malloc(NewCapacityInBytes);

        // Copy the elements over.  No need to run dtors on PODs.
        memcpy(NewElts, this->BeginX, CurSizeBytes);
    } else {
        // If this wasn't grown from the inline copy, grow the allocated space.
        NewElts = realloc(this->BeginX, NewCapacityInBytes);
    }

    this->EndX = (char*)NewElts + CurSizeBytes;
    this->BeginX = NewElts;
    this->CapacityX = (char*)this->BeginX + NewCapacityInBytes;
}
