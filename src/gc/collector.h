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

#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "gc/gc.h"

namespace pyston {

class Box;

namespace gc {

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

    Box* operator->() { return value; }
    Box* get() { return value; }
};

bool isNonheapRoot(void* p);
void registerPythonObject(Box* b);
void invalidateOrderedFinalizerList();

void visitByGCKind(void* p, GCVisitor& visitor);

// Debugging/validation helpers: if a GC should not happen in certain sections (ex during unwinding),
// use these functions to mark that.  This is different from disableGC/enableGC, since it causes an
// assert rather than delaying of the next GC.
void startGCUnexpectedRegion();
void endGCUnexpectedRegion();

class GCVisitorNoRedundancy : public GCVisitor {
public:
    void _visitRedundant(void** ptr_address) override { visit(ptr_address); }
    void _visitRangeRedundant(void** start, void** end) override { visitRange(start, end); }

public:
    virtual ~GCVisitorNoRedundancy() {}

    void visitPotentialRedundant(void* p) override { visitPotential(p); }
    void visitPotentialRangeRedundant(void** start, void** end) override { visitPotentialRange(start, end); }
};

//
// Code to prototype a moving GC.
//

class ReferenceMapWorklist;

#if MOVING_GC
#define MOVING_OVERRIDE override
#else
#define MOVING_OVERRIDE
#endif

#if MOVING_GC
// Bulds the reference map, and also determine which objects cannot be moved.
class GCVisitorPinning : public GCVisitorNoRedundancy {
private:
    ReferenceMapWorklist* worklist;

    void _visit(void** ptr_address) MOVING_OVERRIDE;

public:
    GCVisitorPinning(ReferenceMapWorklist* worklist) : worklist(worklist) {}
    virtual ~GCVisitorPinning() {}

    void visitPotential(void* p) MOVING_OVERRIDE;
};

// Visits the fields and replaces it with new_values if it was equal to old_value.
class GCVisitorReplacing : public GCVisitor {
private:
    void* old_value;
    void* new_value;

    void _visit(void** p) MOVING_OVERRIDE;

public:
    GCVisitorReplacing(void* old_value, void* new_value) : old_value(old_value), new_value(new_value) {}
    virtual ~GCVisitorReplacing() {}

    void visitPotential(void* p) MOVING_OVERRIDE{};
    void visitPotentialRange(void** start, void** end) MOVING_OVERRIDE{};
};

class GCAllocation;
class ReferenceMap {
public:
    // Pinned objects are objects that should not be moved (their pointer value should
    // never change).
    std::unordered_set<GCAllocation*> pinned;

    // Map from objects O to all objects that contain a reference to O.
    std::unordered_map<GCAllocation*, std::vector<GCAllocation*>> references;

    // Track movement (reallocation) of objects.
    std::unordered_map<GCAllocation*, GCAllocation*> moves;
};
#endif
}
}

#endif
