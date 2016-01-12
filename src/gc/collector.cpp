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
#include <llvm/ADT/DenseSet.h>

#include "asm_writing/icinfo.h"
#include "codegen/ast_interpreter.h"
#include "codegen/codegen.h"
#include "codegen/irgen/util.h"
#include "core/common.h"
#include "core/threading.h"
#include "core/types.h"
#include "core/util.h"
#include "gc/heap.h"
#include "runtime/hiddenclass.h"
#include "runtime/objmodel.h"
#include "runtime/types.h"
#include "runtime/util.h"

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

std::vector<Box*> objects_with_ordered_finalizers;

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

int ncollections = 0;

static bool gc_enabled = true;
static bool should_not_reenter_gc = false;

// This is basically a stack. However, for optimization purposes,
// blocks of memory are allocated at once when things need to be pushed.
//
// For performance, this should not have virtual methods.
class ChunkedStack {
protected:
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

public:
    ChunkedStack() { get_chunk(); }
    ~ChunkedStack() {
        RELEASE_ASSERT(end - cur == CHUNK_SIZE, "destroying non-empty ChunkedStack");
        // We always have a block available in case we want to push items onto the TraversalWorklist,
        // but that chunk needs to be released after use to avoid a memory leak.
        release_chunk(start);
    }

    void* pop() {
        if (cur > start)
            return *--cur;

        return pop_chunk_and_item();
    }

    void push(void* p) {
        *cur++ = p;
        if (cur == end) {
            chunks.push_back(start);
            get_chunk();
        }
    }
};
std::vector<void**> ChunkedStack::free_chunks;

enum TraversalType {
    MarkPhase,
    FinalizationOrderingFindReachable,
    FinalizationOrderingRemoveTemporaries,
    MapReferencesPhase,
};

class Worklist {
protected:
    ChunkedStack stack;

public:
    void* next() { return stack.pop(); }
};

class TraversalWorklist : public Worklist {
    TraversalType visit_type;

public:
    TraversalWorklist(TraversalType type) : visit_type(type) {}
    TraversalWorklist(TraversalType type, const std::unordered_set<void*>& roots) : TraversalWorklist(type) {
        for (void* p : roots) {
            ASSERT(!isMarked(GCAllocation::fromUserData(p)), "");
            addWork(p);
        }
    }

    void addWork(void* p) {
        GC_TRACE_LOG("Pushing (%d) %p\n", visit_type, p);
        GCAllocation* al = GCAllocation::fromUserData(p);

        switch (visit_type) {
            case TraversalType::MarkPhase:
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
            case TraversalType::FinalizationOrderingFindReachable:
                if (orderingState(al) == FinalizationState::UNREACHABLE) {
                    GC_TRACE_LOG("%p is now TEMPORARY\n", al->user_data);
                    setOrderingState(al, FinalizationState::TEMPORARY);
                } else if (orderingState(al) == FinalizationState::REACHABLE_FROM_FINALIZER) {
                    GC_TRACE_LOG("%p is now ALIVE\n", al->user_data);
                    setOrderingState(al, FinalizationState::ALIVE);
                } else {
                    return;
                }
                break;
            case TraversalType::FinalizationOrderingRemoveTemporaries:
                if (orderingState(al) == FinalizationState::TEMPORARY) {
                    GC_TRACE_LOG("%p is now REACHABLE_FROM_FINALIZER\n", al->user_data);
                    setOrderingState(al, FinalizationState::REACHABLE_FROM_FINALIZER);
                } else {
                    return;
                }
                break;
            default:
                assert(false);
        }

        stack.push(p);
    }
};

#if MOVING_GC
class ReferenceMapWorklist : public Worklist {
    ReferenceMap* refmap;

public:
    ReferenceMapWorklist(ReferenceMap* refmap) : refmap(refmap) {}
    ReferenceMapWorklist(ReferenceMap* refmap, const std::unordered_set<void*>& roots) : refmap(refmap) {
        for (void* p : roots) {
            addWork(GCAllocation::fromUserData(p), NULL);
        }
    }

    void addWork(GCAllocation* al, GCAllocation* source) {
        assert(refmap);

        auto it = refmap->references.find(al);
        if (it == refmap->references.end()) {
            refmap->references.emplace(al, std::vector<GCAllocation*>());
            auto& vec = refmap->references[al];

            if (source) {
                // We found that there exists a pointer from `source` to `al`
                vec.push_back(source);
            } else {
                // No source => this is a root. We should pin roots.
                refmap->pinned.emplace(al);
            }

            // Pin these types of objects - they are likely to be untracked at
            // this time.
            if (al->kind_id == GCKind::RUNTIME) {
                pin(al);
            } else if (al->kind_id == GCKind::PYTHON) {
                Box* b = (Box*)al->user_data;
                if (b->cls == type_cls || b->cls == module_cls) {
                    pin(al);
                }
            }

            stack.push(al->user_data);
        } else {
            if (source) {
                // We found that there exists a pointer from `source` to `al`
                it->second.push_back(source);
            } else {
                // No source => this is a root. We should pin roots.
                pin(al);
            }
        }
    }

    void pin(GCAllocation* al) { refmap->pinned.emplace(al); }
};
#endif

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
    // only track void* aligned memory
    uintptr_t start_int = (uintptr_t)start;
    uintptr_t end_int = (uintptr_t)end;
    start_int = (start_int + (sizeof(void*) - 1)) & ~(sizeof(void*) - 1);
    end_int -= end_int % sizeof(void*);

    assert(start_int % sizeof(void*) == 0);
    assert(end_int % sizeof(void*) == 0);

    if (end_int > start_int)
        potential_root_ranges.push_back(std::make_pair((void*)start_int, (void*)end_int));
}

extern "C" PyObject* PyGC_AddRoot(PyObject* obj) noexcept {
    if (obj) {
        // Allow duplicates from CAPI code since they shouldn't have to know
        // which objects we already registered as roots:
        registerPermanentRoot(obj, /* allow_duplicates */ true);
    }
    return obj;
}

extern "C" PyObject* PyGC_AddNonHeapRoot(PyObject* obj, int size) noexcept {
    if (obj) {
        registerNonheapRootObject(obj, size);
    }
    return obj;
}

extern "C" void* PyGC_AddPotentialRoot(void* obj, int size) noexcept {
    if (obj)
        registerPotentialRootRange(obj, (char*)obj + size);
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
    return al->user_data == p && al->kind_id == GCKind::PYTHON;
}

void registerPythonObject(Box* b) {
    assert(isValidGCMemory(b));
    auto al = GCAllocation::fromUserData(b);

    assert(al->kind_id == GCKind::CONSERVATIVE || al->kind_id == GCKind::PYTHON);
    al->kind_id = GCKind::PYTHON;

    assert(b->cls);
    if (hasOrderedFinalizer(b->cls)) {
        GC_TRACE_LOG("%p is registered as having an ordered finalizer\n", b);
        objects_with_ordered_finalizers.push_back(b);
    }
}

void invalidateOrderedFinalizerList() {
    static StatCounter sc_us("us_gc_invalidate_ordered_finalizer_list");
    Timer _t("invalidateOrderedFinalizerList", /*min_usec=*/10000);

    auto needToRemove = [](Box* box) -> bool {
        GCAllocation* al = GCAllocation::fromUserData(box);
        if (!hasOrderedFinalizer(box->cls) || hasFinalized(al)) {
            GC_TRACE_LOG("Removing %p from objects_with_ordered_finalizers\n", box);
            return true;
        } else {
            return false;
        }
    };

    objects_with_ordered_finalizers.erase(
        std::remove_if(objects_with_ordered_finalizers.begin(), objects_with_ordered_finalizers.end(), needToRemove),
        objects_with_ordered_finalizers.end());

    long us = _t.end();
    sc_us.log(us);
}

__attribute__((always_inline)) void visitByGCKind(void* p, GCVisitor& visitor) {
    assert(((intptr_t)p) % 8 == 0);

    GCAllocation* al = GCAllocation::fromUserData(p);
    visitor.setSource(al);

    GCKind kind_id = al->kind_id;
    if (kind_id == GCKind::UNTRACKED) {
        // Nothing to do here.
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
    } else if (kind_id == GCKind::RUNTIME) {
        GCAllocatedRuntime* runtime_obj = reinterpret_cast<GCAllocatedRuntime*>(p);
        runtime_obj->gc_visit(&visitor);
    } else {
        RELEASE_ASSERT(0, "Unhandled kind: %d", (int)kind_id);
    }
}

GCRootHandle::GCRootHandle() {
    getRootHandles()->insert(this);
}
GCRootHandle::~GCRootHandle() {
    getRootHandles()->erase(this);
}

void GCVisitor::_visit(void** ptr_address) {
    void* p = *ptr_address;
    if ((uintptr_t)p < SMALL_ARENA_START || (uintptr_t)p >= HUGE_ARENA_START + ARENA_SIZE) {
        ASSERT(!p || isNonheapRoot(p), "%p", p);
        return;
    }

    ASSERT(global_heap.getAllocationFromInteriorPointer(p)->user_data == p, "%p", p);
    worklist->addWork(p);
}

void GCVisitor::_visitRange(void** start, void** end) {
    ASSERT((const char*)end - (const char*)start <= 1000000000, "Asked to scan %.1fGB -- a bug?",
           ((const char*)end - (const char*)start) * 1.0 / (1 << 30));

    assert((uintptr_t)start % sizeof(void*) == 0);
    assert((uintptr_t)end % sizeof(void*) == 0);

    while (start < end) {
        visit(start);
        start++;
    }
}

void GCVisitor::visitPotential(void* p) {
    GCAllocation* a = global_heap.getAllocationFromInteriorPointer(p);
    if (a) {
        worklist->addWork(a->user_data);
    }
}

void GCVisitor::visitPotentialRange(void** start, void** end) {
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

#if MOVING_GC
void GCVisitorPinning::_visit(void** ptr_address) {
    void* p = *ptr_address;
    if ((uintptr_t)p < SMALL_ARENA_START || (uintptr_t)p >= HUGE_ARENA_START + ARENA_SIZE) {
        ASSERT(!p || isNonheapRoot(p), "%p", p);
        return;
    }

    GCAllocation* al = global_heap.getAllocationFromInteriorPointer(p);
    ASSERT(al->user_data == p, "%p", p);
    worklist->addWork(al, source);
}

void GCVisitorPinning::visitPotential(void* p) {
    GCAllocation* a = global_heap.getAllocationFromInteriorPointer(p);
    if (a) {
        worklist->pin(a);
        worklist->addWork(a, source);
    }
}

void GCVisitorReplacing::_visit(void** ptr_address) {
    if (*ptr_address == old_value) {
        *ptr_address = new_value;
    }
}
#endif

static void visitRoots(GCVisitor& visitor) {
    GC_TRACE_LOG("Looking at the stack\n");
    threading::visitAllStacks(&visitor);

    GC_TRACE_LOG("Looking at root handles\n");
    for (auto h : *getRootHandles()) {
        visitor.visit(&h->value);
    }

    GC_TRACE_LOG("Looking at potential root ranges\n");
    for (auto& e : potential_root_ranges) {
        visitor.visitPotentialRange((void**)e.first, (void**)e.second);
    }

    GC_TRACE_LOG("Looking at pending finalization list\n");
    for (auto box : pending_finalization_list) {
        visitor.visit(&box);
    }

    GC_TRACE_LOG("Looking at weakrefs needing callbacks list\n");
    for (auto weakref : weakrefs_needing_callback_list) {
        visitor.visit(&weakref);
    }

    GC_TRACE_LOG("Looking at generated code pointers\n");
    ICInfo::visitGCReferences(&visitor);
#if MOVING_GC
    CompiledFunction::visitAllCompiledFunctions(&visitor);
#endif
}

static void finalizationOrderingFindReachable(Box* obj) {
    static StatCounter sc_marked_objs("gc_marked_object_count_finalizer_ordering");
    static StatCounter sc_us("us_gc_mark_finalizer_ordering_1");
    Timer _t("finalizationOrderingFindReachable", /*min_usec=*/10000);

    TraversalWorklist worklist(TraversalType::FinalizationOrderingFindReachable);
    GCVisitor visitor(&worklist);

    GC_TRACE_LOG("findReachable %p\n", obj);
    worklist.addWork(obj);
    while (void* p = worklist.next()) {
        GC_TRACE_LOG("findReachable, looking at %p\n", p);
        sc_marked_objs.log();

        visitByGCKind(p, visitor);
    }

    long us = _t.end();
    sc_us.log(us);
}

static void finalizationOrderingRemoveTemporaries(Box* obj) {
    static StatCounter sc_us("us_gc_mark_finalizer_ordering_2");
    Timer _t("finalizationOrderingRemoveTemporaries", /*min_usec=*/10000);

    TraversalWorklist worklist(TraversalType::FinalizationOrderingRemoveTemporaries);
    GCVisitor visitor(&worklist);

    GC_TRACE_LOG("removeTemporaries %p\n", obj);
    worklist.addWork(obj);
    while (void* p = worklist.next()) {
        GC_TRACE_LOG("removeTemporaries, looking at %p\n", p);
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
        GC_TRACE_LOG("%p has an ordered finalizer\n", obj);
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
            GC_TRACE_LOG("%p is now pending finalization\n", marked);
            pending_finalization_list.push_back(marked);
        }
    }

    long us = _t.end();
    sc_us.log(us);
}

static void graphTraversalMarking(Worklist& worklist, GCVisitor& visitor) {
    static StatCounter sc_us("us_gc_mark_phase_graph_traversal");
    static StatCounter sc_marked_objs("gc_marked_object_count");
    Timer _t("traversing", /*min_usec=*/10000);

    while (void* p = worklist.next()) {
        sc_marked_objs.log();

        GCAllocation* al = GCAllocation::fromUserData(p);

#if TRACE_GC_MARKING
        if (al->kind_id == GCKind::PYTHON)
            GC_TRACE_LOG("Looking at %s object %p\n", static_cast<Box*>(p)->cls->tp_name, p);
        else
            GC_TRACE_LOG("Looking at non-python allocation %p\n", p);
#endif

        // Won't work once we visit objects in more ways than just marking them.
        assert(isMarked(al) || MOVING_GC);

        visitByGCKind(p, visitor);
    }

    long us = _t.end();
    sc_us.log(us);
}

static void callWeakrefCallback(PyWeakReference* head) {
    if (head->wr_callback) {
        try {
            runtimeCall(head->wr_callback, ArgPassSpec(1), reinterpret_cast<Box*>(head), NULL, NULL, NULL, NULL);
        } catch (ExcInfo e) {
            setCAPIException(e);
            PyErr_WriteUnraisable(head->wr_callback);
        }
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

        GC_TRACE_LOG("Running finalizer for %p\n", box);

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

void markPhase() {
    static StatCounter sc_us("us_gc_mark_phase");
    Timer _t("markPhase", /*min_usec=*/10000);

#ifndef NVALGRIND
    // Have valgrind close its eyes while we do the conservative stack and data scanning,
    // since we'll be looking at potentially-uninitialized values:
    VALGRIND_DISABLE_ERROR_REPORTING;
#endif

    GC_TRACE_LOG("Starting collection %d\n", ncollections);

    GC_TRACE_LOG("Looking at roots\n");
    TraversalWorklist worklist(TraversalType::MarkPhase, roots);
    GCVisitor visitor(&worklist);

    visitRoots(visitor);

    graphTraversalMarking(worklist, visitor);

    // Objects with finalizers cannot be freed in any order. During the call to a finalizer
    // of an object, the finalizer expects the object's references to still point to valid
    // memory. So we root objects whose finalizers need to be called by placing them in a
    // pending finalization list.
    orderFinalizers();

#ifndef NVALGRIND
    VALGRIND_ENABLE_ERROR_REPORTING;
#endif

    long us = _t.end();
    sc_us.log(us);
}

static void sweepPhase(std::vector<Box*>& weakly_referenced, std::vector<BoxedClass*>& classes_to_free) {
    static StatCounter sc_us("us_gc_sweep_phase");
    Timer _t("sweepPhase", /*min_usec=*/10000);

    // we need to use the allocator here because these objects are referenced only here, and calling the weakref
    // callbacks could start another gc
    global_heap.freeUnmarked(weakly_referenced, classes_to_free);

    long us = _t.end();
    sc_us.log(us);
}

static void mapReferencesPhase(ReferenceMap& refmap) {
#if MOVING_GC
    ReferenceMapWorklist worklist(&refmap, roots);
    GCVisitorPinning visitor(&worklist);

    visitRoots(visitor);

    for (auto obj : objects_with_ordered_finalizers) {
        visitor.visit((void**)&obj);
    }

    graphTraversalMarking(worklist, visitor);
#endif
}

#if MOVING_GC
#define MOVE_LOG 1
static FILE* move_log;

static void move(ReferenceMap& refmap, GCAllocation* old_al, size_t size) {
#if MOVE_LOG
    if (!move_log) {
        move_log = fopen("movelog.txt", "w");
    }
#endif

    // Only move objects that are in the reference map (unreachable objects
    // won't be in the reference map).
    if (refmap.pinned.count(old_al) == 0 && refmap.references.count(old_al) > 0) {
        auto& referencing = refmap.references[old_al];
        assert(referencing.size() > 0);

        GCAllocation* new_al = global_heap.forceRelocate(old_al);
        assert(new_al);
        assert(old_al->user_data != new_al->user_data);

#if MOVE_LOG
        // Write the moves that have happened to file, for debugging.
        fprintf(move_log, "%d) %p -> %p\n", ncollections, old_al->user_data, new_al->user_data);
#endif

        for (GCAllocation* referencer : referencing) {
            // If the whatever is pointing to the object we just moved has also been moved,
            // then we need to update the pointer in that moved object.
            if (refmap.moves.count(referencer) > 0) {
                referencer = refmap.moves[referencer];
            }

#if MOVE_LOG
            fprintf(move_log, "    | referencer %p\n", referencer->user_data);
#endif

            assert(referencer->kind_id == GCKind::PYTHON || referencer->kind_id == GCKind::PRECISE
                   || referencer->kind_id == GCKind::RUNTIME);
            GCVisitorReplacing replacer(old_al->user_data, new_al->user_data);
            visitByGCKind(referencer->user_data, replacer);
        }

        assert(refmap.moves.count(old_al) == 0);
        refmap.moves.emplace(old_al, new_al);
    } else if (refmap.pinned.count(old_al) == 0) {
        // TODO: This probably should not happen.
    }
}
#endif

// Move objects around memory randomly. The purpose is to test whether the rest
// of the program is able to support a moving collector (e.g. if all pointers are
// being properly scanned by the GC).
//
// The way it works is very simple.
// 1) Perform a mark phase where for every object, make a list of the location of
//    all pointers to that object (make a reference map).
//    Pin certain types of objects as necessary (e.g. conservatively scanned).
// 2) Reallocate all non-pinned object. Update the value for every pointer locations
//    from the map built in (1)
static void testMoving() {
#if MOVING_GC
    global_heap.prepareForCollection();

    ReferenceMap refmap;
    mapReferencesPhase(refmap);

    // Reallocate (aka 'move') all objects in the small heap to a different
    // location. This is not useful in terms of performance, but it is useful
    // to check if the rest of the program is able to support moving collectors.
    global_heap.forEachSmallArenaReference([&refmap](GCAllocation* al, size_t size) { move(refmap, al, size); });

    global_heap.cleanupAfterCollection();
#endif
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

#if TRACE_GC_MARKING
static void openTraceFp(bool is_pre) {
    if (trace_fp)
        fclose(trace_fp);

    char tracefn_buf[80];
    snprintf(tracefn_buf, sizeof(tracefn_buf), "gc_trace_%d.%04d%s.txt", getpid(), ncollections + is_pre,
             is_pre ? "_pre" : "");
    trace_fp = fopen(tracefn_buf, "w");
    assert(trace_fp);
}

static int _dummy() {
    openTraceFp(true);
    return 0;
}
static int _initializer = _dummy();
#endif

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
    openTraceFp(false);
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

    // Separately keep track of classes that we will be freeing in this collection.
    // We want to make sure that any instances get freed before the class itself gets freed,
    // since the freeing logic can look at the class object.
    // So instead of directly freeing the classes, we stuff them into this vector and then
    // free them at the end.
    std::vector<BoxedClass*> classes_to_free;

    sweepPhase(weakly_referenced, classes_to_free);

    // Handle weakrefs in two passes:
    // - first, find all of the weakref objects whose callbacks we need to call.  we need to iterate
    //   over the garbage-and-corrupt-but-still-alive weakly_referenced list in order to find these objects,
    //   so the gc is not reentrant during this section.  after this we discard that list.
    // - the callbacks are called later, along with the finalizers
    for (auto o : weakly_referenced) {
        assert(isValidGCObject(o));
        GC_TRACE_LOG("%p is weakly referenced\n", o);
        prepareWeakrefCallbacks(o);

        if (PyType_Check(o))
            classes_to_free.push_back(static_cast<BoxedClass*>(o));
        else
            global_heap.free(GCAllocation::fromUserData(o));
    }

    // We want to make sure that classes get freed before their metaclasses.
    // So, while there are still more classes to free, free any classes that are
    // not the metaclass of another class we will free.  Then repeat.
    //
    // Note: our earlier approach of just deferring metaclasses to the next collection is
    // not quite safe, since we will have freed everything that the class refers to.
    while (!classes_to_free.empty()) {
        llvm::DenseSet<BoxedClass*> classes_to_not_free;
        for (auto b : classes_to_free) {
            classes_to_not_free.insert(b->cls);
        }

        std::vector<BoxedClass*> deferred_classes;
        for (auto b : classes_to_free) {
            GC_TRACE_LOG("Dealing with the postponed free of class %p\n", b);
            if (classes_to_not_free.count(b)) {
                deferred_classes.push_back(b);
                continue;
            }
            global_heap._setFree(GCAllocation::fromUserData(b));
        }

        assert(deferred_classes.size() < classes_to_free.size());
        std::swap(deferred_classes, classes_to_free);
    }

    global_heap.cleanupAfterCollection();

#if MOVING_GC
    testMoving();
#endif

#if TRACE_GC_MARKING
    openTraceFp(true);
#endif

    should_not_reenter_gc = false; // end non-reentrant section

    if (VERBOSITY("gc") >= 2)
        printf("Collection #%d done\n\n", ncollections);

    long us = _t.end();
    sc_us.log(us);

    // dumpHeapStatistics();
}

} // namespace gc
} // namespace pyston
