@ Workbench test ROM, ARM7 side: a tail call, as on the ARM9 (testfuncs.s).

    .section .text.wbtest7, "ax", %progbits
    .align 5
    .arm

@ called with BL every frame, so its entry is known
    .global wb7_tail_target
    .type wb7_tail_target, %function
wb7_tail_target:
    add     r0, r0, #5
    add     r0, r0, r0
    bx      lr
    .global wb7_tail_target_end
wb7_tail_target_end:

@ ends in a plain branch into wb7_tail_target: a tail call
    .global wb7_tail_caller
    .type wb7_tail_caller, %function
wb7_tail_caller:
    mov     r0, #9
    b       wb7_tail_target
    .global wb7_tail_caller_end
wb7_tail_caller_end:
