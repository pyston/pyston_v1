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

#ifndef PYSTON_ASMWRITING_MCWRITER_H
#define PYSTON_ASMWRITING_MCWRITER_H

#include "core/ast.h"

namespace pyston {

class BoxedClass;

class MCWriter {
public:
    virtual ~MCWriter() {}

    virtual int numArgRegs() = 0;
    virtual int numTempRegs() = 0;

    // TODO I don't like this method, could be broken down into simpler things
    virtual void emitAlloca(int bytes, int dest_argnum) = 0;

    virtual void emitNop() = 0;
    virtual void emitTrap() = 0;
    virtual void emitAnnotation(int num) = 0;
    virtual void endFastPath(void* success_dest, void* will_relocate_to) = 0;
    virtual void endWithSlowpath() = 0;
    virtual uint8_t* emitCall(void* target, int npushes) = 0;
    virtual void emitGuardFalse() = 0;
    virtual void emitAttrGuard(int argnum, int offset, int64_t val, int npops) = 0;
    virtual void emitGuard(int argnum, int64_t val, int npops) = 0;
    virtual void emitGuardNotEq(int argnum, int64_t val, int npops) = 0;
    virtual void emitMove(int src_argnum, int dest_argnum, int npushed) = 0;
    virtual void emitSetattr(int src_argnum, int dest_argnum, int dest_offset) = 0;
    virtual void emitGetattr(int src_argnum, int src_offset, int dest_argnum) = 0;
    virtual void emitIncattr(int argnum, int offset) = 0;
    virtual void emitPush(int reg) = 0;
    virtual void emitPop(int reg) = 0;
    virtual void emitLoadConst(int reg, int64_t value) = 0;
    virtual void emitCmp(AST_TYPE::AST_TYPE cmp_type, int lhs_argnum, int rhs_argnum, int dest_argnum) = 0;
    virtual void emitToBool(int argnum, int dest_argnum) = 0;
};

void initializePatchpoint(uint8_t* addr, int size);
MCWriter* createMCWriter(uint8_t* addr, int size, int num_temp_regs);
}

#endif
