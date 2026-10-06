/* Bare-metal image loaded at the start of RAM; the stack follows .bss. */
ENTRY(_start)

SECTIONS
{
    . = 0x80000000;
    .text : { *(.text.start) *(.text*) }
    .rodata : { *(.rodata*) *(.srodata*) }
    .data : { *(.data*) *(.sdata*) }
    . = ALIGN(4);
    __bss_start = .;
    .bss : { *(.bss*) *(.sbss*) *(COMMON) }
    . = ALIGN(4);
    __bss_end = .;
    . = . + 0x10000;
    __stack_top = .;
    /DISCARD/ : { *(.comment) *(.riscv.attributes) *(.note*) *(.eh_frame*) }
}
