@ Workbench recording test ROM: functions with a known layout.
@ The integration test checks that exactly these ranges are recorded as
@ ARM code, Thumb code, literal pools and data.

    .section .text.wbtest, "ax", %progbits
    .align 5

@ ---------------------------------------------------------------- ARM
    .arm
    .global wb_arm_func
    .type wb_arm_func, %function
wb_arm_func:
    ldr     r1, =wb_data_table      @ literal pool load
    ldr     r2, [r1]
    ldr     r3, [r1, #4]
    add     r0, r2, r3
    bx      lr
    .pool                           @ 4-byte literal: read, never executed
    .global wb_arm_func_end
wb_arm_func_end:

@ ---------------------------------------------------------------- Thumb
    .align 5
    .thumb
    .global wb_thumb_func
    .thumb_func
    .type wb_thumb_func, %function
wb_thumb_func:
    push    {r4, lr}
    ldr     r4, =wb_data_table
    ldr     r0, [r4, #8]
    add     r0, #1
    bl      wb_thumb_leaf
    pop     {r4, pc}
    .align 2
    .pool
    .global wb_thumb_func_end
wb_thumb_func_end:

    .thumb_func
    .global wb_thumb_leaf
wb_thumb_leaf:
    lsl     r0, r0, #1
    bx      lr
    .align 2
    .global wb_thumb_leaf_end
wb_thumb_leaf_end:

@ ---------------------------------------------------------------- tail call
@ wb_tail_target is called normally first (so it is a known entry), then reached
@ again from wb_tail_caller through a plain branch
    .align 5
    .arm
    .global wb_tail_target
    .type wb_tail_target, %function
wb_tail_target:
    add     r0, r0, #1
    add     r0, r0, r0
    bx      lr
    .global wb_tail_target_end
wb_tail_target_end:

    .global wb_tail_caller
    .type wb_tail_caller, %function
wb_tail_caller:
    mov     r0, #5
    b       wb_tail_target
    .global wb_tail_caller_end
wb_tail_caller_end:

@ wb_kb_target is never called with BL: it is known only once the knowledge store defines it
    .global wb_kb_target
    .type wb_kb_target, %function
wb_kb_target:
    add     r0, r0, #3
    add     r0, r0, r0
    bx      lr
    .global wb_kb_target_end
wb_kb_target_end:

    .global wb_kb_caller
    .type wb_kb_caller, %function
wb_kb_caller:
    mov     r0, #1
    b       wb_kb_target
    .global wb_kb_caller_end
wb_kb_caller_end:

@ a conditional tail call (taken) into a known entry
    .global wb_cond_caller
    .type wb_cond_caller, %function
wb_cond_caller:
    mov     r0, #2
    cmp     r0, #0
    bne     wb_tail_target
    bx      lr
    .global wb_cond_caller_end
wb_cond_caller_end:

@ ---------------------------------------------------------------- data
    .section .data.wbtest, "aw", %progbits
    .align 5
    .global wb_data_table
wb_data_table:
    .word   0x11111111, 0x22222222, 0x33333333, 0x44444444
    .global wb_data_table_end
wb_data_table_end:
