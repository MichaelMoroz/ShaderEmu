/* In place of the kernel: where the machine starts our Linux image, in supervisor mode. */
ENTRY(_start)

SECTIONS
{
    . = 0x82000000;
    .text : { *(.text.start) *(.text) *(.text.[a-oq-z]*) . = ALIGN(4096); *(.text.probe) }
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
