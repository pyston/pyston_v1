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

#include "gc/collector.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>

#include "codegen/ast_interpreter.h"
#include "codegen/codegen.h"
#include "core/common.h"
#include "core/threading.h"
#include "core/types.h"
#include "core/util.h"
#include "gc/heap.h"
#include "runtime/objmodel.h"
#include "runtime/types.h"

#ifndef NVALGRIND
#include "valgrind.h"
#endif

//#undef VERBOSITY
//#define VERBOSITY(x) 2

namespace pyston {
namespace gc {

class TraceStack {
private:
    const int CHUNK_SIZE = 256;
    const int MAX_FREE_CHUNKS = 50;

    std::vector<void**> chunks;
    static std::vector<void**> free_chunks;

    void** cur;
    void** start;
    void** end;

    void get_chunk() {
        if (free_chunks.size()) {
            start = free_chunks.back();
            free_chunks.pop_back();
        } else {
            start = (void**)malloc(sizeof(void*) * CHUNK_SIZE);
        }

        cur = start;
        end = start + CHUNK_SIZE;
    }
    void release_chunk(void** chunk) {
        if (free_chunks.size() == MAX_FREE_CHUNKS)
            free(chunk);
        else
            free_chunks.push_back(chunk);
    }
    void pop_chunk() {
        start = chunks.back();
        chunks.pop_back();
        end = start + CHUNK_SIZE;
        cur = end;
    }

public:
    TraceStack() { get_chunk(); }
    TraceStack(const std::unordered_set<void*>& rhs) {
        get_chunk();
        for (void* p : rhs) {
            assert(!isMarked(GCAllocation::fromUserData(p)));
            push(p);
        }
    }

    void push(void* p) {
        GCAllocation* al = GCAllocation::fromUserData(p);
        if (isMarked(al))
            return;

        setMark(al);

        *cur++ = p;
        if (cur == end) {
            chunks.push_back(start);
            get_chunk();
        }
    }

    void* pop_chunk_and_item() {
        release_chunk(start);
        if (chunks.size()) {
            pop_chunk();
            assert(cur == end);
            return *--cur; // no need for any bounds checks here since we're guaranteed we're CHUNK_SIZE from the start
        }
        return NULL;
    }


    void* pop() {
        if (cur > start)
            return *--cur;

        return pop_chunk_and_item();
    }
};
std::vector<void**> TraceStack::free_chunks;


static std::unordered_set<void*> roots;
void registerPermanentRoot(void* obj, bool allow_duplicates) {
    assert(global_heap.getAllocationFromInteriorPointer(obj));

    // Check for double-registers.  Wouldn't cause any problems, but we probably shouldn't be doing them.
    if (!allow_duplicates)
        ASSERT(roots.count(obj) == 0, "Please only register roots once");

    roots.insert(obj);
}

extern "C" PyObject* PyGC_AddRoot(PyObject* obj) noexcept {
    if (obj) {
        // Allow duplicates from CAPI code since they shouldn't have to know
        // which objects we already registered as roots:
        registerPermanentRoot(obj, /* allow_duplicates */ true);
    }
    return obj;
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

bool isNonheapRoot(void* p) {
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
    ASSERT((const char*)end - (const char*)start <= 1000000000, "Asked to scan %.1fGB -- a bug?",
           ((const char*)end - (const char*)start) * 1.0 / (1 << 30));

    assert((uintptr_t)start % sizeof(void*) == 0);
    assert((uintptr_t)end % sizeof(void*) == 0);

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
    ASSERT((const char*)end - (const char*)start <= 1000000000, "Asked to scan %.1fGB -- a bug?",
           ((const char*)end - (const char*)start) * 1.0 / (1 << 30));

    assert((uintptr_t)start % sizeof(void*) == 0);
    assert((uintptr_t)end % sizeof(void*) == 0);

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
    GCVisitor visitor(&stack);

    threading::visitAllStacks(&visitor);
    gatherInterpreterRoots(&visitor);

    for (void* p : nonheap_roots) {
        Box* b = reinterpret_cast<Box*>(p);
        BoxedClass* cls = b->cls;

        if (cls) {
            ASSERT(cls->gc_visit, "%s", getTypeName(b));
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
        } else if (kind_id == GCKind::PRECISE) {
            uint32_t bytes = al->kind_data;
            visitor.visitRange((void**)p, (void**)((char*)p + bytes));
        } else if (kind_id == GCKind::PYTHON) {
            Box* b = reinterpret_cast<Box*>(p);
            BoxedClass* cls = b->cls;

            if (cls) {
                // The cls can be NULL since we use 'new' to construct them.
                // An arbitrary amount of stuff can happen between the 'new' and
                // the call to the constructor (ie the args get evaluated), which
                // can trigger a collection.
                ASSERT(cls->gc_visit, "%s", getTypeName(b));
                cls->gc_visit(&visitor, b);
            }
        } else if (kind_id == GCKind::HIDDEN_CLASS) {
            HiddenClass* hcls = reinterpret_cast<HiddenClass*>(p);
            hcls->gc_visit(&visitor);
        } else {
            RELEASE_ASSERT(0, "Unhandled kind: %d", (int)kind_id);
        }
    }

#ifndef NVALGRIND
    VALGRIND_ENABLE_ERROR_REPORTING;
#endif
}

static void sweepPhase(std::list<Box*, StlCompatAllocator<Box*>>& weakly_referenced) {
    // we need to use the allocator here because these objects are referenced only here, and calling the weakref
    // callbacks could start another gc
    global_heap.freeUnmarked(weakly_referenced);
}

static bool gc_enabled = true;
bool gcIsEnabled() {
    return gc_enabled;
}
void enableGC() {
    gc_enabled = true;
}
void disableGC() {
    gc_enabled = false;
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
    std::list<Box*, StlCompatAllocator<Box*>> weakly_referenced;
    sweepPhase(weakly_referenced);

    for (auto o : weakly_referenced) {
        PyWeakReference** list = (PyWeakReference**)PyObject_GET_WEAKREFS_LISTPTR(o);
        while (PyWeakReference* head = *list) {
            if (head->wr_object != Py_None) {
                _PyWeakref_ClearRef(head);
                if (head->wr_callback) {

                    runtimeCall(head->wr_callback, ArgPassSpec(1), reinterpret_cast<Box*>(head), NULL, NULL, NULL,
                                NULL);
                    head->wr_callback = NULL;
                }
            }
        }
    }

    if (VERBOSITY("gc") >= 2)
        printf("Collection #%d done\n\n", ncollections);

    long us = _t.end();
    static StatCounter sc_us("gc_collections_us");
    sc_us.log(us);

    // dumpHeapStatistics();
}

} // namespace gc
} // namespace pyston
