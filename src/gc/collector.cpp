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

#ifndef NDEBUG
#define DEBUG 1
#else
#define DEBUG 0
#endif

namespace pyston {
namespace gc {

#if TRACE_GC_MARKING
FILE* trace_fp;
#endif

std::list<Box*> pending_finalization_list;
std::list<PyWeakReference*> weakrefs_needing_callback_list;

static std::unordered_set<void*> roots;
static std::vector<std::pair<void*, void*>> potential_root_ranges;

static std::unordered_set<void*> nonheap_roots;

// Track the highest-addressed nonheap root; the assumption is that the nonheap roots will
// typically all have lower addresses than the heap roots, so this can serve as a cheap
// way to verify it's not a nonheap root (the full check requires a hashtable lookup).
static void* max_nonheap_root = 0;
static void* min_nonheap_root = (void*)~0;

static std::unordered_set<GCRootHandle*>* getRootHandles() {
    static std::unordered_set<GCRootHandle*> root_handles;
    return &root_handles;
}

static bool gc_enabled = true;

static int ncollections = 0;
static bool should_not_reenter_gc = false;

class TraceStack {
protected:
    const int CHUNK_SIZE = 256;
    const int MAX_FREE_CHUNKS = 50;

    std::vector<void**> chunks;
    static std::vector<void**> free_chunks;

    void** cur;
    void** start;
    void** end;

    void* previous_pop = NULL;

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
    virtual ~TraceStack() {}

    // Returns false if we should skip visiting the children.
    // true otherwise.
    virtual bool visit_action(GCAllocation* al) = 0;

    void push(void* p) {
        GC_TRACE_LOG("Pushing %p\n", p);
        GCAllocation* al = GCAllocation::fromUserData(p);

        if (!visit_action(al)) {
            return;
        }

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
        if (cur > start) {
            previous_pop = *--cur;
        } else {
            previous_pop = pop_chunk_and_item();
        }
        return previous_pop;
    }
};
std::vector<void**> TraceStack::free_chunks;

class MarkStack : public TraceStack {
public:
    MarkStack(const std::unordered_set<void*>& root_handles) : TraceStack() {
        for (void* p : root_handles) {
            assert(!isMarked(GCAllocation::fromUserData(p)));
            push(p);
        }
    }

    virtual bool visit_action(GCAllocation* al) {
// Use this to print the directed edges of the GC graph traversal.
// i.e. print every a > b where a is a pointer and b is something a references
#if 0
        if (previous_pop) {
            GCAllocation* source_allocation = GCAllocation::fromUserData(previous_pop);
            if (source_allocation->kind_id == GCKind::PYTHON) {
                printf("(%s) ", ((Box*)previous_pop)->cls->tp_name);
            }
            printf("%p > %p", previous_pop, al->user_data);
        } else {
            printf("source %p", al->user_data);
        }

        if (al->kind_id == GCKind::PYTHON) {
            printf(" (%s)", ((Box*)al->user_data)->cls->tp_name);
        }
        printf("\n");

#endif

        if (isMarked(al)) {
            return false;
        } else {
            setMark(al);
            return true;
        }
    }
};

class FirstPhaseStack : public TraceStack {
public:
    FirstPhaseStack() : TraceStack() {}

    virtual bool visit_action(GCAllocation* al) {
        if (orderingState(al) == FinalizationState::UNREACHABLE) {
            setOrderingState(al, FinalizationState::TEMPORARY);
            return true;
        } else if (orderingState(al) == FinalizationState::REACHABLE_FROM_FINALIZER) {
            setOrderingState(al, FinalizationState::ALIVE);
            return true;
        } else {
            return false;
        }
    }
};

class SecondPhaseStack : public TraceStack {
public:
    SecondPhaseStack() : TraceStack() {}

    virtual bool visit_action(GCAllocation* al) {
        if (orderingState(al) == FinalizationState::TEMPORARY) {
            setOrderingState(al, FinalizationState::REACHABLE_FROM_FINALIZER);
            return true;
        } else {
            return false;
        }
    }
};

void registerPermanentRoot(void* obj, bool allow_duplicates) {
    assert(global_heap.getAllocationFromInteriorPointer(obj));

    // Check for double-registers.  Wouldn't cause any problems, but we probably shouldn't be doing them.
    if (!allow_duplicates)
        ASSERT(roots.count(obj) == 0, "Please only register roots once");

    roots.insert(obj);
}

void deregisterPermanentRoot(void* obj) {
    assert(global_heap.getAllocationFromInteriorPointer(obj));
    ASSERT(roots.count(obj), "");
    roots.erase(obj);
}

void registerPotentialRootRange(void* start, void* end) {
    potential_root_ranges.push_back(std::make_pair(start, end));
}

extern "C" PyObject* PyGC_AddRoot(PyObject* obj) noexcept {
    if (obj) {
        // Allow duplicates from CAPI code since they shouldn't have to know
        // which objects we already registered as roots:
        registerPermanentRoot(obj, /* allow_duplicates */ true);
    }
    return obj;
}

void registerNonheapRootObject(void* obj, int size) {
    // I suppose that things could work fine even if this were true, but why would it happen?
    assert(global_heap.getAllocationFromInteriorPointer(obj) == NULL);
    assert(nonheap_roots.count(obj) == 0);

    nonheap_roots.insert(obj);
    registerPotentialRootRange(obj, ((uint8_t*)obj) + size);

    max_nonheap_root = std::max(obj, max_nonheap_root);
    min_nonheap_root = std::min(obj, min_nonheap_root);
}

bool isNonheapRoot(void* p) {
    if (p > max_nonheap_root || p < min_nonheap_root)
        return false;
    return nonheap_roots.count(p) != 0;
}

bool isValidGCMemory(void* p) {
    return isNonheapRoot(p) || (global_heap.getAllocationFromInteriorPointer(p)->user_data == p);
}

bool isValidGCObject(void* p) {
    if (isNonheapRoot(p))
        return true;
    GCAllocation* al = global_heap.getAllocationFromInteriorPointer(p);
    if (!al)
        return false;
    return al->user_data == p && (al->kind_id == GCKind::CONSERVATIVE_PYTHON || al->kind_id == GCKind::PYTHON);
}

void setIsPythonObject(Box* b) {
    assert(isValidGCMemory(b));
    auto al = GCAllocation::fromUserData(b);

    if (al->kind_id == GCKind::CONSERVATIVE) {
        al->kind_id = GCKind::CONSERVATIVE_PYTHON;
    } else {
        assert(al->kind_id == GCKind::PYTHON);
    }
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
    if (!p)
        return;

    if (isNonheapRoot(p)) {
        return;
    } else {
        ASSERT(global_heap.getAllocationFromInteriorPointer(p)->user_data == p, "%p", p);
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
#if TRACE_GC_MARKING
        if (global_heap.getAllocationFromInteriorPointer(*start)) {
            if (*start >= (void*)HUGE_ARENA_START)
                GC_TRACE_LOG("Found conservative reference to huge object %p from %p\n", *start, start);
            else if (*start >= (void*)LARGE_ARENA_START && *start < (void*)HUGE_ARENA_START)
                GC_TRACE_LOG("Found conservative reference to large object %p from %p\n", *start, start);
            else
                GC_TRACE_LOG("Found conservative reference to %p from %p\n", *start, start);
        }
#endif

        visitPotential(*start);
        start++;
    }
}

void visitByGCKind(void* p, GCVisitor& visitor) {
    assert(((intptr_t)p) % 8 == 0);
    GCAllocation* al = GCAllocation::fromUserData(p);

    GCKind kind_id = al->kind_id;
    if (kind_id == GCKind::UNTRACKED) {
        // Nothing here.
    } else if (kind_id == GCKind::CONSERVATIVE || kind_id == GCKind::CONSERVATIVE_PYTHON) {
        uint32_t bytes = al->kind_data;
        if (DEBUG >= 2) {
            global_heap.assertSmallArenaContains(p, bytes);
        }
        visitor.visitPotentialRange((void**)p, (void**)((char*)p + bytes));
    } else if (kind_id == GCKind::PRECISE) {
        uint32_t bytes = al->kind_data;
        if (DEBUG >= 2) {
            global_heap.assertSmallArenaContains(p, bytes);
        }
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

static void getMarkPhaseRoots(GCVisitor& visitor) {
    GC_TRACE_LOG("Looking at the stack\n");
    threading::visitAllStacks(&visitor);

    GC_TRACE_LOG("Looking at root handles\n");
    for (auto h : *getRootHandles()) {
        visitor.visit(h->value);
    }

    GC_TRACE_LOG("Looking at potential root ranges\n");
    for (auto& e : potential_root_ranges) {
        visitor.visitPotentialRange((void* const*)e.first, (void* const*)e.second);
    }

    GC_TRACE_LOG("Looking at pending finalization list\n");
    for (auto box : pending_finalization_list) {
        visitor.visit(box);
    }

    GC_TRACE_LOG("Looking at weakrefs needing callbacks list\n");
    for (auto weakref : weakrefs_needing_callback_list) {
        visitor.visit(weakref);
    }
}

static void traverseHeapForFinalizers(std::vector<Box*>& objs_with_ordered_finalizers, GCVisitor& visitor) {
    static StatCounter sc_us("us_gc_traverse_heap");
    Timer _t("traverseHeap", /*min_usec=*/10000);

    global_heap.traverseForFinalizers(objs_with_ordered_finalizers, visitor);

    long us = _t.end();
    sc_us.log(us);
}

static void graphTraversalMarking(std::shared_ptr<MarkStack> stack, GCVisitor& visitor) {
    static StatCounter sc_us("us_gc_mark_phase_graph_traversal");
    static StatCounter sc_marked_objs("gc_marked_object_count");
    Timer _t("traversing", /*min_usec=*/10000);

    while (void* p = stack->pop()) {
        sc_marked_objs.log();
        GCAllocation* al = GCAllocation::fromUserData(p);

#if TRACE_GC_MARKING
        if (al->kind_id == GCKind::PYTHON || al->kind_id == GCKind::CONSERVATIVE_PYTHON)
            GC_TRACE_LOG("Looking at %s object %p\n", static_cast<Box*>(p)->cls->tp_name, p);
        else
            GC_TRACE_LOG("Looking at non-python allocation %p\n", p);
#endif

        assert(isMarked(al));
        visitByGCKind(p, visitor);
    }

    long us = _t.end();
    sc_us.log(us);
}

static void finalizationOrderingFirstPass(Box* obj) {
    static StatCounter sc_marked_objs("gc_marked_object_count_finalizer_ordering");
    static StatCounter sc_us("us_gc_mark_finalizer_ordering_1");
    Timer _t("finalizationOrderingFirstPass", /*min_usec=*/10000);

    std::shared_ptr<FirstPhaseStack> stack = std::make_shared<FirstPhaseStack>();
    GCVisitor visitor(stack);

    stack->push(obj);
    while (void* p = stack->pop()) {
        sc_marked_objs.log();

        visitByGCKind(p, visitor);
    }

    long us = _t.end();
    sc_us.log(us);
}

static void finalizationOrderingSecondPass(Box* obj) {
    static StatCounter sc_us("us_gc_mark_finalizer_ordering_2");
    Timer _t("finalizationOrderingSecondPass", /*min_usec=*/10000);

    std::shared_ptr<SecondPhaseStack> stack = std::make_shared<SecondPhaseStack>();
    GCVisitor visitor(stack);

    stack->push(obj);
    while (void* p = stack->pop()) {
        GCAllocation* al = GCAllocation::fromUserData(p);
        assert(orderingState(al) != FinalizationState::UNREACHABLE);
        visitByGCKind(p, visitor);
    }

    long us = _t.end();
    sc_us.log(us);
}

// Implementation of PyPy's finalization ordering algorithm:
// http://pypy.readthedocs.org/en/latest/discussion/finalizer-order.html
static void finalizationOrderingPhase(std::vector<Box*>& objects_with_ordered_finalizers) {
    static StatCounter sc_us("us_gc_finalization_ordering_phase");
    Timer _t("finalizationOrderingPhase", /*min_usec=*/10000);

    std::vector<Box*> finalizer_marked;

    for (Box* obj : objects_with_ordered_finalizers) {
        GCAllocation* al = GCAllocation::fromUserData(obj);

        // We are only interested in object with finalizers that need to be
        // garbage-collected.
        if (orderingState(al) == FinalizationState::UNREACHABLE) {
            // Unordered finalizers don't block the objects they reference from being finalized.
            assert(hasOrderedFinalizer(obj));
            finalizer_marked.push_back(obj);
            finalizationOrderingFirstPass(obj);
            finalizationOrderingSecondPass(obj);
        }
    }

    for (Box* marked : finalizer_marked) {
        GCAllocation* al = GCAllocation::fromUserData(marked);

        FinalizationState state = orderingState(al);
        assert(state == FinalizationState::REACHABLE_FROM_FINALIZER || state == FinalizationState::ALIVE);

        if (state == FinalizationState::REACHABLE_FROM_FINALIZER) {
            pending_finalization_list.push_back(marked);
        }
    }

    long us = _t.end();
    sc_us.log(us);
}

static void markPhase() {
    static StatCounter sc_us("us_gc_mark_phase");
    Timer _t("marking", /*min_usec=*/10000);

    std::shared_ptr<MarkStack> stack = std::make_shared<MarkStack>(roots);
    GCVisitor visitor(stack);
    std::vector<Box*> objects_with_ordered_finalizers;

#ifndef NVALGRIND
    // Have valgrind close its eyes while we do the conservative stack and data scanning,
    // since we'll be looking at potentially-uninitialized values:
    VALGRIND_DISABLE_ERROR_REPORTING;
#endif

#if TRACE_GC_MARKING
#if 1 // separate log file per collection
    char tracefn_buf[80];
    snprintf(tracefn_buf, sizeof(tracefn_buf), "gc_trace_%03d.txt", ncollections);
    trace_fp = fopen(tracefn_buf, "w");
#else // overwrite previous log file with each collection
    trace_fp = fopen("gc_trace.txt", "w");
#endif
#endif
    GC_TRACE_LOG("Starting collection %d\n", ncollections);

    // Starting points of graph traversal.
    GC_TRACE_LOG("Looking at roots\n");
    getMarkPhaseRoots(visitor);

    // This is different from vanilla mark-and-sweep. We need to do a full heap traversal:
    // 1) In Python, all classes are themselves objects. We want to mark the class objects
    //    of any object on the heap, even if the object isn't reachable - this is because
    //    when we free and object, we might still need to look at it's class.
    // 2) Objects that have finalizers have special constraints so we need a list of them.
    traverseHeapForFinalizers(objects_with_ordered_finalizers, visitor);

    graphTraversalMarking(stack, visitor);

    // Objects with finalizers cannot be freed in any order. During the call to a finalizer
    // of an object, the finalizer expects the object's references to still point to valid
    // memory. So we root objects whose finalizers need to be called by placing them in a
    // pending finalization list.
    finalizationOrderingPhase(objects_with_ordered_finalizers);

#if TRACE_GC_MARKING
    fclose(trace_fp);
    trace_fp = NULL;
#endif

#ifndef NVALGRIND
    VALGRIND_ENABLE_ERROR_REPORTING;
#endif

    long us = _t.end();
    sc_us.log(us);
}

static void sweepPhase(std::vector<Box*>& weakly_referenced) {
    static StatCounter sc_us("us_gc_sweep_phase");
    Timer _t("sweepPhase", /*min_usec=*/10000);

    global_heap.freeUnmarked(weakly_referenced);

    long us = _t.end();
    sc_us.log(us);
}

void callPendingFinalizers() {
    static StatCounter sc_us_finalizer("us_gc_finalizercalls");
    static StatCounter sc_us_weakref("us_gc_weakrefcalls");

    // An object can be resurrected in the finalizer code. So when we call a finalizer, we
    // mark the finalizer as having been called, but the object is only freed in another
    // GC pass (objects whose finalizers have been called are treated the same as objects
    // without finalizers).
    Timer _timer_finalizer("calling finalizers", /*min_usec=*/10000);
    while (!pending_finalization_list.empty()) {
        Box* box = pending_finalization_list.front();
        pending_finalization_list.pop_front();

        assert(isValidGCObject(box));

        if (isWeaklyReferenced(box)) {
            // Callbacks for weakly-referenced objects with finalizers (if any), followed by call to finalizers.
            PyWeakReference** list = (PyWeakReference**)PyObject_GET_WEAKREFS_LISTPTR(box);
            while (PyWeakReference* head = *list) {
                assert(isValidGCObject(head));
                if (head->wr_object != Py_None) {
                    assert(head->wr_object == box);
                    _PyWeakref_ClearRef(head);

                    if (head->wr_callback) {
                        runtimeCall(head->wr_callback, ArgPassSpec(1), reinterpret_cast<Box*>(head), NULL, NULL, NULL,
                                    NULL);
                        head->wr_callback = NULL;
                    }
                }
            }
        }

        finalize(box);
    }

    sc_us_finalizer.log(_timer_finalizer.end());

    // Callbacks for weakly-referenced objects without finalizers.
    Timer _timer_weakref("calling weakref callbacks", /*min_usec=*/10000);
    while (!weakrefs_needing_callback_list.empty()) {
        PyWeakReference* head = weakrefs_needing_callback_list.front();
        weakrefs_needing_callback_list.pop_front();

        if (head->wr_callback) {
            runtimeCall(head->wr_callback, ArgPassSpec(1), reinterpret_cast<Box*>(head), NULL, NULL, NULL, NULL);
            head->wr_callback = NULL;
        }
    }

    sc_us_weakref.log(_timer_weakref.end());

    assert(pending_finalization_list.empty());
}

bool gcIsEnabled() {
    return gc_enabled;
}

void enableGC() {
    gc_enabled = true;
}

void disableGC() {
    gc_enabled = false;
}

void startGCUnexpectedRegion() {
    RELEASE_ASSERT(!should_not_reenter_gc, "");
    should_not_reenter_gc = true;
}

void endGCUnexpectedRegion() {
    RELEASE_ASSERT(should_not_reenter_gc, "");
    should_not_reenter_gc = false;
}

void runCollection() {
    static StatCounter sc("gc_collections");
    sc.log();

    UNAVOIDABLE_STAT_TIMER(t0, "us_timer_gc_collection");

    ncollections++;

    if (VERBOSITY("gc") >= 2)
        printf("Collection #%d\n", ncollections);

    // The bulk of the GC work is not reentrant-safe.
    // In theory we should never try to reenter that section, but it's happened due to bugs,
    // which show up as very-hard-to-understand gc issues.
    // So keep track if we're in the non-reentrant section and abort if we try to go back in.
    // We could also just skip the collection if we're currently in the gc, but I think if we
    // run into this case it's way more likely that it's a bug than something we should ignore.
    RELEASE_ASSERT(!should_not_reenter_gc, "");
    should_not_reenter_gc = true; // begin non-reentrant section

    Timer _t("collecting", /*min_usec=*/10000);

    markPhase();

    // The sweep phase will not free weakly-referenced objects, so that we can inspect their
    // weakrefs_list.  We want to defer looking at those lists until the end of the sweep phase,
    // since the deallocation of other objects (namely, the weakref objects themselves) can affect
    // those lists, and we want to see the final versions.
    std::vector<Box*> weakly_referenced;
    sweepPhase(weakly_referenced);

    // Free weakly-referenced objects without finalizers and make a list of callbacks to call.
    // They will be called later outside of GC when it is safe to do so. Weakrefs callbacks are
    // just like finalizers, except that they don't impose ordering.
    for (auto o : weakly_referenced) {
        assert(isValidGCObject(o));
        PyWeakReference** list = (PyWeakReference**)PyObject_GET_WEAKREFS_LISTPTR(o);
        while (PyWeakReference* head = *list) {
            assert(isValidGCObject(head));
            if (head->wr_object != Py_None) {
                assert(head->wr_object == o);
                _PyWeakref_ClearRef(head);

                if (head->wr_callback) {
                    weakrefs_needing_callback_list.push_back(head);
                }
            }
        }

        // We didn't finalize these objects because we skipped them during sweep phase.
        finalizeIfUnordered(o);

        global_heap.free(GCAllocation::fromUserData(o));
    }

    should_not_reenter_gc = false; // end non-reentrant section

    if (VERBOSITY("gc") >= 2)
        printf("Collection #%d done\n\n", ncollections);

    long us = _t.end();
    static StatCounter sc_us("us_gc_collections");
    sc_us.log(us);

    // dumpHeapStatistics();
}

} // namespace gc
} // namespace pyston
