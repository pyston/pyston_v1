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

// This functions sets up a fixed stack frame which we use to detect ASTInterpreter frames
// and which makes it easy retrieve the passed ASTInterpreter pointer (stored at frame_ptr-8). 
// It's written in ASM to make sure the stack layout keeps beeing the same and that nothing gets inlined.
// Our unwinder treats this function specialy.

// Box* executeInnerAndSetupFrame(ASTInterpreter& interpreter, CFGBlock* start_block, AST_stmt* start_at)
.text
.globl executeInnerAndSetupFrame
.type executeInnerAndSetupFrame,@function
.align 16
executeInnerAndSetupFrame:
    .cfi_startproc
    push %rbp
    .cfi_def_cfa_offset     16
    .cfi_offset             rbp,-16
    mov %rsp, %rbp
    .cfi_def_cfa_register   rbp
    sub $16, %rsp
    mov %rdi, -8(%rbp)
    call executeInnerFromASM
    leave
    .cfi_def_cfa            rsp,8
    ret
    .cfi_endproc
.size executeInnerAndSetupFrame,.-executeInnerAndSetupFrame

.section .note.GNU-stack,"",%progbits // we don't need executable stack

