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

#ifndef PYSTON_GC_COLLECTOR_H
#define PYSTON_GC_COLLECTOR_H

#include <deque>
#include <list>
#include <vector>

#include "core/types.h"

namespace pyston {
namespace gc {

#define TRACE_GC_MARKING 0
#if TRACE_GC_MARKING
extern FILE* trace_fp;
#define GC_TRACE_LOG(...) fprintf(pyston::gc::trace_fp, __VA_ARGS__)
#else
#define GC_TRACE_LOG(...)
#endif

extern std::deque<Box*> pending_finalization_list;
extern std::deque<PyWeakReference*> weakrefs_needing_callback_list;

// Mark this gc-allocated object as being a root, even if there are no visible references to it.
// (Note: this marks the gc allocation itself, not the pointer that points to one.  For that, use
// a GCRootHandle)
void registerPermanentRoot(void* root_obj, bool allow_duplicates = false);
void deregisterPermanentRoot(void* root_obj);

// Register an object that was not allocated through this collector, as a root for this collector.
// The motivating usecase is statically-allocated PyTypeObject objects, which are full Python objects
// even if they are not heap allocated.
// This memory will be scanned conservatively.
void registerNonheapRootObject(void* obj, int size);

void registerPotentialRootRange(void* start, void* end);

// If you want to have a static root "location" where multiple values could be stored, use this:
class GCRootHandle {
public:
    Box* value;

    GCRootHandle();
    ~GCRootHandle();

    void operator=(Box* b) { value = b; }

    operator Box*() { return value; }
    Box* operator->() { return value; }
};

void callPendingDestructionLogic();
void runCollection();

// Python programs are allowed to pause the GC.  This is supposed to pause automatic GC,
// but does not seem to pause manual calls to gc.collect().  So, callers should check gcIsEnabled(),
// if appropriate, before calling runCollection().
bool gcIsEnabled();
void disableGC();
void enableGC();

// These are mostly for debugging:
bool isValidGCMemory(void* p); // if p is a valid gc-allocated pointer (or a non-heap root)
bool isValidGCObject(void* p); // whether p is valid gc memory and is set to have Python destructor semantics applied
bool isNonheapRoot(void* p);
void registerPythonObject(Box* b);
void invalidateOrderedFinalizerList();

// Debugging/validation helpers: if a GC should not happen in certain sections (ex during unwinding),
// use these functions to mark that.  This is different from disableGC/enableGC, since it causes an
// assert rather than delaying of the next GC.
void startGCUnexpectedRegion();
void endGCUnexpectedRegion();
}
}

#endif
