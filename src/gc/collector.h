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

#ifndef PYSTON_GC_COLLECTOR_H
#define PYSTON_GC_COLLECTOR_H

#include <vector>

#include "core/types.h"

namespace pyston {
namespace gc {

#define MARK_BIT 0x1

inline void setMark(GCObjectHeader* header) {
    header->gc_flags |= MARK_BIT;
}

inline void clearMark(GCObjectHeader* header) {
    header->gc_flags &= ~MARK_BIT;
}

inline bool isMarked(GCObjectHeader* header) {
    return (header->gc_flags & MARK_BIT) != 0;
}

#undef MARK_BIT

class TraceStack {
private:
    std::vector<void*> v;

public:
    void pushall(void* const* start, void* const* end) { v.insert(v.end(), start, end); }

    void push(void* p) { v.push_back(p); }

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

class TraceStackGCVisitor : public GCVisitor {
private:
    bool isValid(void* p);

    void _visit(void* p);

public:
    TraceStack* stack;
    TraceStackGCVisitor(TraceStack* stack) : stack(stack) {}

    void visit(void* p) override;
    void visitRange(void* const* start, void* const* end) override;
    void visitPotential(void* p) override;
    void visitPotentialRange(void* const* start, void* const* end) override;
};

// Call it a "root obj" because this function takes the pointer to the object, not a pointer
// to a storage location where we might store different objects.
// ie this only works for constant roots, and not out-of-gc-knowledge storage locations
// (that should be registerStaticRootPtr)
void registerStaticRootObj(void* root_obj);
void runCollection();

// If you want to have a static root "location" where multiple values could be stored, use this:
class StaticRootHandle {
public:
    Box* value;

    StaticRootHandle();
    ~StaticRootHandle();

    void operator=(Box* b) { value = b; }

    operator Box*() { return value; }
};
}
}

#endif
