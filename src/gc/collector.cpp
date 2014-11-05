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

#include "gc/collector.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>

#include "codegen/codegen.h"
#include "core/common.h"
#include "core/threading.h"
#include "core/types.h"
#include "core/util.h"
#include "gc/heap.h"
#include "gc/root_finder.h"

#ifndef NVALGRIND
#include "valgrind.h"
#endif

//#undef VERBOSITY
//#define VERBOSITY(x) 2

namespace pyston {
namespace gc {

class TraceStack {
private:
    std::vector<void*> v;

public:
    TraceStack() {}
    TraceStack(const std::vector<void*>& rhs) {
        for (void* p : rhs) {
            assert(!isMarked(GCAllocation::fromUserData(p)));
            push(p);
        }
    }

    void push(void* p) {
        GCAllocation* al = GCAllocation::fromUserData(p);

        if (!isMarked(al)) {
            setMark(al);

            v.push_back(p);
        }
    }

    int size() { return v.size(); }

    void reserve(int num) { v.reserve(num + v.size()); }

    void* pop() {
        if (v.size()) {
            void* r = v.back();
            v.pop_back();
            return r;
        }
        return NULL;
    }
};

static std::vector<void*> roots;
void registerPermanentRoot(void* obj) {
    assert(global_heap.getAllocationFromInteriorPointer(obj));
    roots.push_back(obj);

#ifndef NDEBUG
    // Check for double-registers.  Wouldn't cause any problems, but we probably shouldn't be doing them.
    static std::unordered_set<void*> roots;
    ASSERT(roots.count(obj) == 0, "Please only register roots once");
    roots.insert(obj);
#endif
}

static std::unordered_set<void*> nonheap_roots;
// Track the highest-addressed nonheap root; the assumption is that the nonheap roots will
// typically all have lower addresses than the heap roots, so this can serve as a cheap
// way to verify it's not a nonheap root (the full check requires a hashtable lookup).
static void* max_nonheap_root = 0;
void registerNonheapRootObject(void* obj) {
    // I suppose that things could work fine even if this were true, but why would it happen?
    assert(global_heap.getAllocationFromInteriorPointer(obj) == NULL);
    assert(nonheap_roots.count(obj) == 0);

    nonheap_roots.insert(obj);

    max_nonheap_root = std::max(obj, max_nonheap_root);
}

static bool isNonheapRoot(void* p) {
    return p <= max_nonheap_root && nonheap_roots.count(p) != 0;
}

bool isValidGCObject(void* p) {
    return isNonheapRoot(p) || (global_heap.getAllocationFromInteriorPointer(p)->user_data == p);
}

static std::unordered_set<GCRootHandle*>* getRootHandles() {
    static std::unordered_set<GCRootHandle*> root_handles;
    return &root_handles;
}

GCRootHandle::GCRootHandle() {
    getRootHandles()->insert(this);
}
GCRootHandle::~GCRootHandle() {
    getRootHandles()->erase(this);
}



bool GCVisitor::isValid(void* p) {
    return global_heap.getAllocationFromInteriorPointer(p) != NULL;
}

void GCVisitor::visit(void* p) {
    if (isNonheapRoot(p)) {
        return;
    } else {
        assert(global_heap.getAllocationFromInteriorPointer(p)->user_data == p);
        stack->push(p);
    }
}

void GCVisitor::visitRange(void* const* start, void* const* end) {
    while (start < end) {
        visit(*start);
        start++;
    }
}

void GCVisitor::visitPotential(void* p) {
    GCAllocation* a = global_heap.getAllocationFromInteriorPointer(p);
    if (a) {
        visit(a->user_data);
    }
}

void GCVisitor::visitPotentialRange(void* const* start, void* const* end) {
    while (start < end) {
        visitPotential(*start);
        start++;
    }
}

static void markPhase() {
#ifndef NVALGRIND
    // Have valgrind close its eyes while we do the conservative stack and data scanning,
    // since we'll be looking at potentially-uninitialized values:
    VALGRIND_DISABLE_ERROR_REPORTING;
#endif

    TraceStack stack(roots);
    collectStackRoots(&stack);

    GCVisitor visitor(&stack);

    for (void* p : nonheap_roots) {
        Box* b = reinterpret_cast<Box*>(p);
        BoxedClass* cls = b->cls;

        if (cls) {
            ASSERT(cls->gc_visit, "%s", getTypeName(b)->c_str());
            cls->gc_visit(&visitor, b);
        }
    }

    for (auto h : *getRootHandles()) {
        visitor.visitPotential(h->value);
    }

    // if (VERBOSITY()) printf("Found %d roots\n", stack.size());
    while (void* p = stack.pop()) {
        assert(((intptr_t)p) % 8 == 0);
        GCAllocation* al = GCAllocation::fromUserData(p);

        assert(isMarked(al));

        // printf("Marking + scanning %p\n", p);

        GCKind kind_id = al->kind_id;
        if (kind_id == GCKind::UNTRACKED) {
            continue;
        } else if (kind_id == GCKind::CONSERVATIVE) {
            uint32_t bytes = al->kind_data;
            visitor.visitPotentialRange((void**)p, (void**)((char*)p + bytes));
        } else if (kind_id == GCKind::PYTHON) {
            Box* b = reinterpret_cast<Box*>(p);
            BoxedClass* cls = b->cls;

            if (cls) {
                // The cls can be NULL since we use 'new' to construct them.
                // An arbitrary amount of stuff can happen between the 'new' and
                // the call to the constructor (ie the args get evaluated), which
                // can trigger a collection.
                ASSERT(cls->gc_visit, "%s", getTypeName(b)->c_str());
                cls->gc_visit(&visitor, b);
            }
        } else {
            RELEASE_ASSERT(0, "Unhandled kind: %d", (int)kind_id);
        }
    }

#ifndef NVALGRIND
    VALGRIND_ENABLE_ERROR_REPORTING;
#endif
}

static void sweepPhase() {
    global_heap.freeUnmarked();
}

static int ncollections = 0;
void runCollection() {
    static StatCounter sc("gc_collections");
    sc.log();

    ncollections++;

    if (VERBOSITY("gc") >= 2)
        printf("Collection #%d\n", ncollections);

    Timer _t("collecting", /*min_usec=*/10000);

    markPhase();
    sweepPhase();
    if (VERBOSITY("gc") >= 2)
        printf("Collection #%d done\n\n", ncollections);

    long us = _t.end();
    static StatCounter sc_us("gc_collections_us");
    sc_us.log(us);

    // dumpHeapStatistics();
}

} // namespace gc
} // namespace pyston
