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
