ENTRY(_start)
SECTIONS
{
    . = 0x80080000;
    .text : { *(.text.boot) *(.text.vectors) *(.text .text.*) }
    .rodata : { *(.rodata .rodata.*) }
    .data : { *(.data .data.*) }
    . = ALIGN(16);
    _edata = .;
    __bss_start = .;
    .bss : { *(.bss .bss.*) *(COMMON) . = ALIGN(16); }
    __bss_end = .;
    . = ALIGN(4096);
    . += 0x10000;
    _stack_top = .;
    _image_size = _edata - 0x80080000;
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.gnu*) }
}
