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

#ifndef PYSTON_RUNTIME_ICS_H
#define PYSTON_RUNTIME_ICS_H

#include "core/common.h"
#include "runtime/objmodel.h"

namespace pyston {

class ICInfo;

class RuntimeIC {
private:
    void* addr;
    void* eh_frame_addr;

    std::unique_ptr<ICInfo> icinfo;

    RuntimeIC(const RuntimeIC&) = delete;
    void operator=(const RuntimeIC&) = delete;

protected:
    RuntimeIC(void* addr, int num_slots, int slot_size);
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
    CallattrIC() : RuntimeIC((void*)callattr, 1, 160) {}

    Box* call(Box* obj, const std::string* attr, CallattrFlags flags, ArgPassSpec spec, Box* arg0, Box* arg1, Box* arg2,
              Box** args, const std::vector<const std::string*>* keyword_names) {
        return (Box*)call_ptr(obj, attr, flags, spec, arg0, arg1, arg2, args, keyword_names);
    }
};

class BinopIC : public RuntimeIC {
public:
    BinopIC() : RuntimeIC((void*)binop, 1, 160) {}

    Box* call(Box* lhs, Box* rhs, int op_type) { return (Box*)call_ptr(lhs, rhs, op_type); }
};

class NonzeroIC : public RuntimeIC {
public:
    NonzeroIC() : RuntimeIC((void*)nonzero, 1, 40) {}

    bool call(Box* obj) { return call_bool(obj); }
};

} // namespace pyston

#endif
