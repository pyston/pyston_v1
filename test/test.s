	.text
    movq $123, 4(%rsp)
    movq $123, 4(%rcx)
    movsd %xmm0, %xmm1
    movsd %xmm0, %xmm9
    sub $123, %rsp
    sub $129, %rsp
