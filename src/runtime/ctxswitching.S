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

// This functions are influenced by boost context written by Oliver Kowalke

// Context* makeContext(void* stack_top, void (*start_func)(intptr_t))
.text
.globl makeContext
.type makeContext,@function
.align 16
makeContext:
    leaq  -64(%rdi), %rax   // make space for the init Context (56bytes + 8byte alignment)
    movq  %rsi, 48(%rax)    // set context->ip to the start_func param
    ret                     // return new adjusted top of stack
.size makeContext,.-makeContext


// void swapContext(Context** old_context, Context* new_context, intptr_t arg)
.globl swapContext
.type swapContext,@function
.align 16
swapContext:
    pushq  %rbp             // save regs
    pushq  %rbx
    pushq  %r15
    pushq  %r14
    pushq  %r13
    pushq  %r12

    movq  %rsp, (%rdi)      // store current SP in *old_context
    movq  %rsi, %rsp        // point SP to new_context

    movq  %rdx, %rdi        // let arg be the first argument to start_func

    popq  %r12              // restore regs
    popq  %r13
    popq  %r14
    popq  %r15
    popq  %rbx
    popq  %rbp

    popq  %r8               // load new_context->ip into r8
    jmp  *%r8               // jump to context->ip
.size swapContext,.-swapContext

.section .note.GNU-stack,"",%progbits // we don't need executable stack
