/* Entry for Redmi Note 7 (lavender) boot test loader. Loaded by ABL as an ARM64 "Image" at 0x80080000. */
    .section .text.boot, "ax"
    .global _start
_start:
    b       reset
    .word   0
    .quad   0x80000            /* text_offset */
    .quad   _image_size        /* image_size */
    .quad   0xa                /* flags: 4K pages, placement anywhere */
    .quad   0
    .quad   0
    .quad   0
    .word   0x644d5241         /* "ARM\x64" */
    .word   0
reset:
    msr     daifset, #0xf
    mov     x19, x0            /* DTB pointer from ABL */
    msr     spsel, #1          /* make sure we run on SP_EL1 so same-EL exceptions use this stack */
    adrp    x1, vectors
    add     x1, x1, :lo12:vectors
    msr     vbar_el1, x1
    adrp    x1, _stack_top
    add     x1, x1, :lo12:_stack_top
    mov     sp, x1
    adrp    x0, __bss_start
    add     x0, x0, :lo12:__bss_start
    adrp    x1, __bss_end
    add     x1, x1, :lo12:__bss_end
1:  cmp     x0, x1
    b.hs    2f
    str     xzr, [x0], #8
    b       1b
2:  mov     x0, x19
    bl      kmain
3:  wfi
    b       3b

/* u64 smc_call(u64 fn, u64 a1, u64 a2, u64 a3): AAPCS args == SMCCC args */
    .global smc_call
smc_call:
    smc     #0
    ret

/* PSCI CPU_ON entry: x0 = context_id = top of this CPU's stack */
    .global secondary_entry
secondary_entry:
    msr     daifset, #0xf
    mov     sp, x0
    bl      secondary_main
4:  wfe
    b       4b

/* ---- exception vectors: save everything, call exc_handler(type, esr, far, elr), restore, eret ---- */
    .macro  ventry idx
    .align  7
    stp     x0, x1, [sp, #-16]!
    mov     x0, #\idx
    b       exc_entry
    .endm

    .section .text.vectors, "ax"
    .align  11
    .global vectors
vectors:
    ventry  0
    ventry  1
    ventry  2
    ventry  3
    ventry  4
    ventry  5
    ventry  6
    ventry  7
    ventry  8
    ventry  9
    ventry  10
    ventry  11
    ventry  12
    ventry  13
    ventry  14
    ventry  15

exc_entry:
    stp     x2, x3, [sp, #-16]!
    stp     x4, x5, [sp, #-16]!
    stp     x6, x7, [sp, #-16]!
    stp     x8, x9, [sp, #-16]!
    stp     x10, x11, [sp, #-16]!
    stp     x12, x13, [sp, #-16]!
    stp     x14, x15, [sp, #-16]!
    stp     x16, x17, [sp, #-16]!
    stp     x18, x19, [sp, #-16]!
    stp     x20, x21, [sp, #-16]!
    stp     x22, x23, [sp, #-16]!
    stp     x24, x25, [sp, #-16]!
    stp     x26, x27, [sp, #-16]!
    stp     x28, x29, [sp, #-16]!
    str     x30, [sp, #-16]!
    mrs     x1, esr_el1
    mrs     x2, far_el1
    mrs     x3, elr_el1
    bl      exc_handler
    ldr     x30, [sp], #16
    ldp     x28, x29, [sp], #16
    ldp     x26, x27, [sp], #16
    ldp     x24, x25, [sp], #16
    ldp     x22, x23, [sp], #16
    ldp     x20, x21, [sp], #16
    ldp     x18, x19, [sp], #16
    ldp     x16, x17, [sp], #16
    ldp     x14, x15, [sp], #16
    ldp     x12, x13, [sp], #16
    ldp     x10, x11, [sp], #16
    ldp     x8, x9, [sp], #16
    ldp     x6, x7, [sp], #16
    ldp     x4, x5, [sp], #16
    ldp     x2, x3, [sp], #16
    ldp     x0, x1, [sp], #16
    eret
