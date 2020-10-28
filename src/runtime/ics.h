// Copyright (c) 2014-2016 Dropbox, Inc.
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

#ifndef PYSTON_RUNTIME_ICS_H
#define PYSTON_RUNTIME_ICS_H

#include "codegen/unwinding.h" // RegisterEHFrame
#include "core/common.h"
#include "runtime/objmodel.h"

namespace pyston {

class ICInfo;

class RuntimeIC {
private:
    void* addr; // points to function start not the start of the allocated memory block.

    RegisterEHFrame register_eh_frame;
    std::unique_ptr<ICInfo> icinfo;

    RuntimeIC(const RuntimeIC&) = delete;
    void operator=(const RuntimeIC&) = delete;

protected:
    RuntimeIC(void* addr, int total_size);
    ~RuntimeIC();

    template <class... Args> uint64_t call_int(Args... args) {
        return reinterpret_cast<uint64_t (*)(Args...)>(this->addr)(args...);
    }

    template <class... Args> bool call_bool(Args... args) {
        return reinterpret_cast<bool (*)(Args...)>(this->addr)(args...);
    }

    template <class... Args> void* call_ptr(Args... args) {
        return reinterpret_cast<void* (*)(Args...)>(this->addr)(args...);
    }

    template <class... Args> double call_double(Args... args) {
        return reinterpret_cast<double (*)(Args...)>(this->addr)(args...);
    }
};

class CallattrIC : public RuntimeIC {
public:
    CallattrIC() : RuntimeIC((void*)callattr, 512) {}

    Box* call(Box* obj, BoxedString* attr, CallattrFlags flags, Box* arg0, Box* arg1, Box* arg2, Box** args,
              BoxedTuple* keyword_names) {
        return (Box*)call_ptr(obj, attr, flags, arg0, arg1, arg2, args, keyword_names);
    }
};

class CallattrCapiIC : public RuntimeIC {
public:
    CallattrCapiIC() : RuntimeIC((void*)callattrCapi, 512) {}

    Box* call(Box* obj, BoxedString* attr, CallattrFlags flags, Box* arg0, Box* arg1, Box* arg2, Box** args,
              BoxedTuple* keyword_names) {
        return (Box*)call_ptr(obj, attr, flags, arg0, arg1, arg2, args, keyword_names);
    }
};


class BinopIC : public RuntimeIC {
public:
    BinopIC() : RuntimeIC((void*)binop, 512) {}

    Box* call(Box* lhs, Box* rhs, int op_type) { return (Box*)call_ptr(lhs, rhs, op_type); }
};

class NonzeroIC : public RuntimeIC {
public:
    NonzeroIC() : RuntimeIC((void*)nonzero, 512) {}

    bool call(Box* obj) { return call_bool(obj); }
};

template <class ICType, unsigned cache_size> class RuntimeICCache {
private:
    struct PerCallerIC {
        void* caller_addr;
        std::shared_ptr<ICType> ic;
    };
    PerCallerIC ics[cache_size];
    unsigned next_to_replace;

    RuntimeICCache(const RuntimeICCache&) = delete;
    void operator=(const RuntimeICCache&) = delete;

    PerCallerIC* findBestSlotToReplace() {
        // search for an unassigned slot
        for (unsigned i = 0; i < cache_size; ++i) {
            if (!ics[i].caller_addr)
                return &ics[i];
        }

        PerCallerIC* ic = &ics[next_to_replace];
        ++next_to_replace;
        if (next_to_replace >= cache_size)
            next_to_replace = 0;
        return ic;
    }

public:
    RuntimeICCache() : next_to_replace(0) {
        for (unsigned i = 0; i < cache_size; ++i)
            ics[i].caller_addr = 0;
    }

    std::shared_ptr<ICType> getIC(void* caller_addr) {
        assert(caller_addr);

        // try to find a cached IC for the caller
        for (unsigned i = 0; i < cache_size; ++i) {
            if (ics[i].caller_addr == caller_addr)
                return ics[i].ic;
        }

        // could not find a cached runtime IC, create new one and save it
        PerCallerIC* slot_to_replace = findBestSlotToReplace();
        std::shared_ptr<ICType> ic = std::make_shared<ICType>();
        slot_to_replace->caller_addr = caller_addr;
        slot_to_replace->ic = ic;
        return ic;
    }
};

} // namespace pyston

#endif
