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
#include "runtime/hiddenclass.h"
#include "runtime/objmodel.h"
#include "runtime/types.h"

#ifndef NVALGRIND
#include "valgrind.h"
#endif

namespace pyston {
namespace gc {

#if TRACE_GC_MARKING
FILE* trace_fp;
#endif

std::deque<Box*> pending_finalization_list;
std::deque<PyWeakReference*> weakrefs_needing_callback_list;

std::list<Box*> objects_with_ordered_finalizers;

static std::unordered_set<void*> roots;
static std::vector<std::pair<void*, void*>> potential_root_ranges;

// BoxedClasses in the program that are still needed.
static std::unordered_set<BoxedClass*> class_objects;

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

static int ncollections = 0;

static bool gc_enabled = true;
static bool should_not_reenter_gc = false;

enum TraceStackType {
    MarkPhase,
    FinalizationOrderingFindReachable,
    FinalizationOrderingRemoveTemporaries,
};

class TraceStack {
private:
    const int CHUNK_SIZE = 256;
    const int MAX_FREE_CHUNKS = 50;

    std::vector<void**> chunks;
    static std::vector<void**> free_chunks;

    void** cur;
    void** start;
    void** end;

    TraceStackType visit_type;

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
    TraceStack(TraceStackType type) : visit_type(type) { get_chunk(); }
    TraceStack(TraceStackType type, const std::unordered_set<void*>& roots) : visit_type(type) {
        get_chunk();
        for (void* p : roots) {
            ASSERT(!isMarked(GCAllocation::fromUserData(p)), "");
            push(p);
        }
    }
    ~TraceStack() {
        RELEASE_ASSERT(end - cur == CHUNK_SIZE, "destroying non-empty TraceStack");

        // We always have a block available in case we want to push items onto the TraceStack,
        // but that chunk needs to be released after use to avoid a memory leak.
        release_chunk(start);
    }

    void push(void* p) {
        GC_TRACE_LOG("Pushing %p\n", p);
        GCAllocation* al = GCAllocation::fromUserData(p);

        switch (visit_type) {
            case TraceStackType::MarkPhase:
// Use this to print the directed edges of the GC graph traversal.
// i.e. print every a -> b where a is a pointer and b is something a references
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
                    return;
                } else {
                    setMark(al);
                }
                break;
            // See PyPy's finalization ordering algorithm:
            // http://pypy.readthedocs.org/en/latest/discussion/finalizer-order.html
            case TraceStackType::FinalizationOrderingFindReachable:
                if (orderingState(al) == FinalizationState::UNREACHABLE) {
                    setOrderingState(al, FinalizationState::TEMPORARY);
                } else if (orderingState(al) == FinalizationState::REACHABLE_FROM_FINALIZER) {
                    setOrderingState(al, FinalizationState::ALIVE);
                } else {
                    return;
                }
                break;
            case TraceStackType::FinalizationOrderingRemoveTemporaries:
                if (orderingState(al) == FinalizationState::TEMPORARY) {
                    setOrderingState(al, FinalizationState::REACHABLE_FROM_FINALIZER);
                } else {
                    return;
                }
                break;
            default:
                assert(false);
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
        } else {
            // We emptied the stack, but we should prepare a new chunk in case another item
            // gets added onto the stack.
            get_chunk();
            return NULL;
        }
    }


    void* pop() {
        if (cur > start)
            return *--cur;

        return pop_chunk_and_item();
    }
};
std::vector<void**> TraceStack::free_chunks;

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

void registerPythonObject(Box* b) {
    assert(isValidGCMemory(b));
    auto al = GCAllocation::fromUserData(b);

    if (al->kind_id == GCKind::CONSERVATIVE) {
        al->kind_id = GCKind::CONSERVATIVE_PYTHON;
    } else {
        assert(al->kind_id == GCKind::PYTHON);
    }

    assert(b->cls);
    if (hasOrderedFinalizer(b->cls)) {
        objects_with_ordered_finalizers.push_back(b);
    }
    if (PyType_Check(b)) {
        class_objects.insert((BoxedClass*)b);
    }
}

void invalidateOrderedFinalizerList() {
    static StatCounter sc_us("us_gc_invalidate_ordered_finalizer_list");
    Timer _t("invalidateOrderedFinalizerList", /*min_usec=*/10000);

    for (auto iter = objects_with_ordered_finalizers.begin(); iter != objects_with_ordered_finalizers.end();) {
        Box* box = *iter;
        GCAllocation* al = GCAllocation::fromUserData(box);

        if (!hasOrderedFinalizer(box->cls) || hasFinalized(al)) {
            // Cleanup.
            iter = objects_with_ordered_finalizers.erase(iter);
        } else {
            ++iter;
        }
    }

    long us = _t.end();
    sc_us.log(us);
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
    if ((uintptr_t)p < SMALL_ARENA_START || (uintptr_t)p >= HUGE_ARENA_START + ARENA_SIZE) {
        ASSERT(!p || isNonheapRoot(p), "%p", p);
        return;
    }

    ASSERT(global_heap.getAllocationFromInteriorPointer(p)->user_data == p, "%p", p);
    stack->push(p);
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

static __attribute__((always_inline)) void visitByGCKind(void* p, GCVisitor& visitor) {
    assert(((intptr_t)p) % 8 == 0);

    GCAllocation* al = GCAllocation::fromUserData(p);

    GCKind kind_id = al->kind_id;
    if (kind_id == GCKind::UNTRACKED) {
        // Nothing to do here.
    } else if (kind_id == GCKind::CONSERVATIVE || kind_id == GCKind::CONSERVATIVE_PYTHON) {
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

static void markRoots(GCVisitor& visitor) {
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

static void finalizationOrderingFindReachable(Box* obj) {
    static StatCounter sc_marked_objs("gc_marked_object_count_finalizer_ordering");
    static StatCounter sc_us("us_gc_mark_finalizer_ordering_1");
    Timer _t("finalizationOrderingFindReachable", /*min_usec=*/10000);

    TraceStack stack(TraceStackType::FinalizationOrderingFindReachable);
    GCVisitor visitor(&stack);

    stack.push(obj);
    while (void* p = stack.pop()) {
        sc_marked_objs.log();

        visitByGCKind(p, visitor);
    }

    long us = _t.end();
    sc_us.log(us);
}

static void finalizationOrderingRemoveTemporaries(Box* obj) {
    static StatCounter sc_us("us_gc_mark_finalizer_ordering_2");
    Timer _t("finalizationOrderingRemoveTemporaries", /*min_usec=*/10000);

    TraceStack stack(TraceStackType::FinalizationOrderingRemoveTemporaries);
    GCVisitor visitor(&stack);

    stack.push(obj);
    while (void* p = stack.pop()) {
        GCAllocation* al = GCAllocation::fromUserData(p);
        assert(orderingState(al) != FinalizationState::UNREACHABLE);
        visitByGCKind(p, visitor);
    }

    long us = _t.end();
    sc_us.log(us);
}

// Implementation of PyPy's finalization ordering algorithm:
// http://pypy.readthedocs.org/en/latest/discussion/finalizer-order.html
static void orderFinalizers() {
    static StatCounter sc_us("us_gc_finalization_ordering");
    Timer _t("finalizationOrdering", /*min_usec=*/10000);

    std::vector<Box*> finalizer_marked;

    for (Box* obj : objects_with_ordered_finalizers) {
        GCAllocation* al = GCAllocation::fromUserData(obj);

        // We are only interested in object with finalizers that need to be garbage-collected.
        if (orderingState(al) == FinalizationState::UNREACHABLE) {
            assert(hasOrderedFinalizer(obj->cls));

            finalizer_marked.push_back(obj);
            finalizationOrderingFindReachable(obj);
            finalizationOrderingRemoveTemporaries(obj);
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

static void graphTraversalMarking(TraceStack& stack, GCVisitor& visitor) {
    static StatCounter sc_us("us_gc_mark_phase_graph_traversal");
    static StatCounter sc_marked_objs("gc_marked_object_count");
    Timer _t("traversing", /*min_usec=*/10000);

    while (void* p = stack.pop()) {
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

static void callWeakrefCallback(PyWeakReference* head) {
    if (head->wr_callback) {
        runtimeCall(head->wr_callback, ArgPassSpec(1), reinterpret_cast<Box*>(head), NULL, NULL, NULL, NULL);
        head->wr_callback = NULL;
    }
}

static void callPendingFinalizers() {
    static StatCounter sc_us_finalizer("us_gc_finalizercalls");
    Timer _timer_finalizer("calling finalizers", /*min_usec=*/10000);

    bool initially_empty = pending_finalization_list.empty();

    // An object can be resurrected in the finalizer code. So when we call a finalizer, we
    // mark the finalizer as having been called, but the object is only freed in another
    // GC pass (objects whose finalizers have been called are treated the same as objects
    // without finalizers).
    while (!pending_finalization_list.empty()) {
        Box* box = pending_finalization_list.front();
        pending_finalization_list.pop_front();

        ASSERT(isValidGCObject(box), "objects to be finalized should still be alive");

        if (isWeaklyReferenced(box)) {
            // Callbacks for weakly-referenced objects with finalizers (if any), followed by call to finalizers.
            PyWeakReference** list = (PyWeakReference**)PyObject_GET_WEAKREFS_LISTPTR(box);
            while (PyWeakReference* head = *list) {
                assert(isValidGCObject(head));
                if (head->wr_object != Py_None) {
                    assert(head->wr_object == box);
                    _PyWeakref_ClearRef(head);

                    callWeakrefCallback(head);
                }
            }
        }

        finalize(box);
        ASSERT(isValidGCObject(box), "finalizing an object should not free the object");
    }

    if (!initially_empty) {
        invalidateOrderedFinalizerList();
    }

    sc_us_finalizer.log(_timer_finalizer.end());
}

static void callPendingWeakrefCallbacks() {
    static StatCounter sc_us_weakref("us_gc_weakrefcalls");
    Timer _timer_weakref("calling weakref callbacks", /*min_usec=*/10000);

    // Callbacks for weakly-referenced objects without finalizers.
    while (!weakrefs_needing_callback_list.empty()) {
        PyWeakReference* head = weakrefs_needing_callback_list.front();
        weakrefs_needing_callback_list.pop_front();

        callWeakrefCallback(head);
    }

    sc_us_weakref.log(_timer_weakref.end());
}

void callPendingDestructionLogic() {
    static bool callingPending = false;

    // Calling finalizers is likely going to lead to another call to allowGLReadPreemption
    // and reenter callPendingDestructionLogic, so we'd really only be calling
    // one finalizer per function call to callPendingFinalizers/WeakrefCallbacks. The purpose
    // of this boolean is to avoid that.
    if (!callingPending) {
        callingPending = true;

        callPendingFinalizers();
        callPendingWeakrefCallbacks();

        callingPending = false;
    }
}

static void prepareWeakrefCallbacks(Box* box) {
    PyWeakReference** list = (PyWeakReference**)PyObject_GET_WEAKREFS_LISTPTR(box);
    while (PyWeakReference* head = *list) {
        assert(isValidGCObject(head));
        if (head->wr_object != Py_None) {
            assert(head->wr_object == box);
            _PyWeakref_ClearRef(head);

            if (head->wr_callback) {
                weakrefs_needing_callback_list.push_back(head);
            }
        }
    }
}

static void markPhase() {
    static StatCounter sc_us("us_gc_mark_phase");
    Timer _t("markPhase", /*min_usec=*/10000);

#ifndef NVALGRIND
    // Have valgrind close its eyes while we do the conservative stack and data scanning,
    // since we'll be looking at potentially-uninitialized values:
    VALGRIND_DISABLE_ERROR_REPORTING;
#endif

    GC_TRACE_LOG("Starting collection %d\n", ncollections);

    GC_TRACE_LOG("Looking at roots\n");
    TraceStack stack(TraceStackType::MarkPhase, roots);
    GCVisitor visitor(&stack);

    markRoots(visitor);

    graphTraversalMarking(stack, visitor);

    // Some classes might be unreachable. Unfortunately, we have to keep them around for
    // one more collection, because during the sweep phase, instances of unreachable
    // classes might still end up looking at the class. So we visit those unreachable
    // classes remove them from the list of class objects so that it can be freed
    // in the next collection.
    std::vector<BoxedClass*> classes_to_remove;
    for (BoxedClass* cls : class_objects) {
        GCAllocation* al = GCAllocation::fromUserData(cls);
        if (!isMarked(al)) {
            visitor.visit(cls);
            classes_to_remove.push_back(cls);
        }
    }

    // We added new objects to the stack again from visiting classes so we nee to do
    // another (mini) traversal.
    graphTraversalMarking(stack, visitor);

    for (BoxedClass* cls : classes_to_remove) {
        class_objects.erase(cls);
    }

    // The above algorithm could fail if we have a class and a metaclass -- they might
    // both have been added to the classes to remove. In case that happens, make sure
    // that the metaclass is retained for at least another collection.
    for (BoxedClass* cls : classes_to_remove) {
        class_objects.insert(cls->cls);
    }

    // Objects with finalizers cannot be freed in any order. During the call to a finalizer
    // of an object, the finalizer expects the object's references to still point to valid
    // memory. So we root objects whose finalizers need to be called by placing them in a
    // pending finalization list.
    orderFinalizers();

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

    // we need to use the allocator here because these objects are referenced only here, and calling the weakref
    // callbacks could start another gc
    global_heap.freeUnmarked(weakly_referenced);

    long us = _t.end();
    sc_us.log(us);
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
    static StatCounter sc_us("us_gc_collections");
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

#if TRACE_GC_MARKING
#if 1 // separate log file per collection
    char tracefn_buf[80];
    snprintf(tracefn_buf, sizeof(tracefn_buf), "gc_trace_%d.%03d.txt", getpid(), ncollections);
    trace_fp = fopen(tracefn_buf, "w");
#else // overwrite previous log file with each collection
    trace_fp = fopen("gc_trace.txt", "w");
#endif
#endif

    global_heap.prepareForCollection();

    // Finalizers might have been called since the last GC.
    // Normally we invalidate the list everytime we call a batch of objects with finalizers.
    // However, there are some edge cases where that isn't sufficient, such as a GC being triggered
    // inside a finalizer call. To be safe, it's better to invalidate the list again.
    invalidateOrderedFinalizerList();

    markPhase();

    // The sweep phase will not free weakly-referenced objects, so that we can inspect their
    // weakrefs_list.  We want to defer looking at those lists until the end of the sweep phase,
    // since the deallocation of other objects (namely, the weakref objects themselves) can affect
    // those lists, and we want to see the final versions.
    std::vector<Box*> weakly_referenced;
    sweepPhase(weakly_referenced);

    // Handle weakrefs in two passes:
    // - first, find all of the weakref objects whose callbacks we need to call.  we need to iterate
    //   over the garbage-and-corrupt-but-still-alive weakly_referenced list in order to find these objects,
    //   so the gc is not reentrant during this section.  after this we discard that list.
    // - the callbacks are called later, along with the finalizers
    for (auto o : weakly_referenced) {
        assert(isValidGCObject(o));
        prepareWeakrefCallbacks(o);
        global_heap.free(GCAllocation::fromUserData(o));
    }

#if TRACE_GC_MARKING
    fclose(trace_fp);
    trace_fp = NULL;
#endif

    should_not_reenter_gc = false; // end non-reentrant section

    global_heap.cleanupAfterCollection();

    if (VERBOSITY("gc") >= 2)
        printf("Collection #%d done\n\n", ncollections);

    long us = _t.end();
    sc_us.log(us);

    // dumpHeapStatistics();
}

} // namespace gc
} // namespace pyston
