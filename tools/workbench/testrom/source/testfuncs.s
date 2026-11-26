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

@ ---------------------------------------------------------------- bl that is not a call
@ Thumb code uses bl as a long jump too (it reaches further than b). The recorder sees a bl set the
@ return address either way; only whether the callee comes back tells a call from a jump.
    .align 5
    .thumb
    .global wb_blbranch
    .thumb_func
    .type wb_blbranch, %function
wb_blbranch:
    push    {r4, lr}
    mov     r4, #0
    .global wb_blbranch_loop
    .thumb_func
wb_blbranch_loop:
    add     r4, #1
    cmp     r4, #2
    bne     3f
    ldr     r0, =wb_blbranch_after  @ second pass: a computed jump, as a switch's jump table makes,
    mov     pc, r0                  @ to the instruction after the bl: its return address. That is
                                    @ not the bl's call coming back
3:  cmp     r4, #3
    bge     2f
    bl      wb_blbranch_loop        @ first pass: a jump written as bl, never returns here
    .global wb_blbranch_after
wb_blbranch_after:
    b       wb_blbranch_loop        @ a branch back into this function: not a tail call
2:  bl      wb_blbranch_sub         @ a real call into the middle of this function: it returns
    pop     {r4, pc}
    .global wb_blbranch_sub
    .thumb_func
wb_blbranch_sub:
    add     r0, r4, #1
    bx      lr
    .align 2
    .pool
    .global wb_blbranch_end
wb_blbranch_end:
    .arm

@ ---------------------------------------------------------------- data
    .section .data.wbtest, "aw", %progbits
    .align 5
    .global wb_data_table
wb_data_table:
    .word   0x11111111, 0x22222222, 0x33333333, 0x44444444
    .global wb_data_table_end
wb_data_table_end:
